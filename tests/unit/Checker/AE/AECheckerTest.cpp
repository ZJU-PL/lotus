#include "AECheckerTestSupport.h"

TEST_F(AECheckerTest, ConstantArrayBufferOverflow) {
  const char *source = R"(
    define void @test_overflow() {
      %arr = alloca [10 x i32], align 4
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 15
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, VariableLengthArray) {
  const char *source = R"(
    define void @test_vla(i32 %n) {
      %arr = alloca i32, i32 %n
      %gep = getelementptr inbounds i32, i32* %arr, i64 0
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, NestedGEP) {
  const char *source = R"(
    define void @test_nested_gep() {
      %arr = alloca [10 x [20 x i32]], align 4
      %gep1 = getelementptr inbounds [10 x [20 x i32]], [10 x [20 x i32]]* %arr, i64 0, i64 5
      %gep2 = getelementptr inbounds [20 x i32], [20 x i32]* %gep1, i64 0, i64 15
      store i32 42, i32* %gep2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
  // Nested GEP should correctly track offset from base
  // Offset = 5 * (20 * 4) + 15 * 4 = 400 + 60 = 460 bytes
  // Array size = 10 * 20 * 4 = 800 bytes, so this is safe
}
TEST_F(AECheckerTest, NullPointerDeref) {
  const char *source = R"(
    define void @test_null_deref() {
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      %val = load i32, i32* %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, NullPointerDerefGEP) {
  const char *source = R"(
    define void @test_null_gep() {
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      %gep = getelementptr inbounds i32, i32* %p, i64 5
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, MemcpyBufferOverflow) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
    
    define void @test_memcpy_overflow() {
      %dst = alloca [10 x i8], align 1
      %src = alloca [20 x i8], align 1
      %dst_ptr = bitcast [10 x i8]* %dst to i8*
      %src_ptr = bitcast [20 x i8]* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst_ptr, i8* %src_ptr, i64 15, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, MemcpyInteriorPointerOverflow) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @test_memcpy_tail_overflow() {
      %dst = alloca [10 x i8], align 1
      %src = alloca [4 x i8], align 1
      %dst_base = bitcast [10 x i8]* %dst to i8*
      %dst_tail = getelementptr inbounds i8, i8* %dst_base, i64 8
      %src_ptr = bitcast [4 x i8]* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst_tail, i8* %src_ptr, i64 4, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, MemcpyInteriorPointerWithinRemainingCapacity) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @test_memcpy_tail_safe() {
      %dst = alloca [10 x i8], align 1
      %src = alloca [2 x i8], align 1
      %dst_base = bitcast [10 x i8]* %dst to i8*
      %dst_tail = getelementptr inbounds i8, i8* %dst_base, i64 8
      %src_ptr = bitcast [2 x i8]* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst_tail, i8* %src_ptr, i64 2, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, StrcpyBufferOverflow) {
  const char *source = R"(
    declare i8* @strcpy(i8*, i8*)
    
    define void @test_strcpy_overflow() {
      %dst = alloca [10 x i8], align 1
      %src = alloca [20 x i8], align 1
      %dst_ptr = bitcast [10 x i8]* %dst to i8*
      %src_ptr = bitcast [20 x i8]* %src to i8*
      call i8* @strcpy(i8* %dst_ptr, i8* %src_ptr)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, SafeBufferAccess) {
  const char *source = R"(
    define void @test_safe_access() {
      %arr = alloca [10 x i32], align 4
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 5
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, ComplexControlFlow) {
  const char *source = R"(
    define void @test_control_flow(i32 %n) {
      %arr = alloca [10 x i32], align 4
      %cmp = icmp slt i32 %n, 10
      br i1 %cmp, label %if_true, label %if_false
    
    if_true:
      %n64 = sext i32 %n to i64
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 %n64
      store i32 42, i32* %gep
      br label %end
    
    if_false:
      %gep2 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 15
      store i32 42, i32* %gep2
      br label %end
    
    end:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, UnsignedLessThanTrueBranchRefinement) {
  const char *source = R"(
    define void @test_ult_true_branch(i32 %n) {
    entry:
      %cmp = icmp ult i32 %n, 2
      br i1 %cmp, label %safe, label %checked

    safe:
      %arr = alloca [2 x i32], align 4
      %n64 = sext i32 %n to i64
      %gep = getelementptr inbounds [2 x i32], [2 x i32]* %arr, i64 0, i64 %n64
      store i32 1, i32* %gep
      ret void

    checked:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, VLATrackedSize) {
  const char *source = R"(
    define void @test_vla_tracked(i32 %n) {
      %n_clamped = call i32 @llvm.smin.i32(i32 %n, i32 100)
      %arr = alloca i32, i32 %n_clamped
      %gep = getelementptr inbounds i32, i32* %arr, i64 50
      store i32 42, i32* %gep
      ret void
    }
    
    declare i32 @llvm.smin.i32(i32, i32)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, HeapAllocationOverflow) {
  const char *source = R"(
    declare i8* @malloc(i64)
    
    define void @test_heap_overflow() {
      %ptr = call i8* @malloc(i64 10)
      %gep = getelementptr inbounds i8, i8* %ptr, i64 15
      store i8 42, i8* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, StructFieldAccess) {
  const char *source = R"(
    %struct.Test = type { i32, i32, i32 }
    
    define void @test_struct() {
      %s = alloca %struct.Test, align 4
      %gep = getelementptr inbounds %struct.Test, %struct.Test* %s, i64 0, i32 2
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, LoopBufferAccess) {
  const char *source = R"(
    define void @test_loop() {
      %arr = alloca [10 x i32], align 4
      br label %loop
    
    loop:
      %i = phi i32 [ 0, %0 ], [ %i_next, %loop ]
      %i64 = sext i32 %i to i64
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 %i64
      store i32 42, i32* %gep
      %i_next = add i32 %i, 1
      %cmp = icmp slt i32 %i_next, 10
      br i1 %cmp, label %loop, label %exit
    
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AECheckerTest, LoopCarriedIndexReachesInclusiveBound) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %arr = alloca [10 x i32], align 4
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %i64 = sext i32 %i to i64
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 %i64
      store i32 %i, i32* %gep
      %next = add i32 %i, 1
      %cmp = icmp sle i32 %next, 10
      br i1 %cmp, label %loop, label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}

TEST_F(AECheckerTest, NegativeArrayIndexIsOverflow) {
  const char *source = R"(
    define i32 @main() {
      %arr = alloca [10 x i32], align 4
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 -1
      store i32 1, i32* %gep
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AECheckerTest, OffsetThroughLoadedPointerRetainsBaseOffset) {
  const char *source = R"(
    define i32 @main() {
      %arr = alloca [10 x i32], align 4
      %base = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 0
      %mid = getelementptr inbounds i32, i32* %base, i64 5
      %slot = alloca i32*, align 8
      store i32* %mid, i32** %slot, align 8
      %p = load i32*, i32** %slot, align 8
      %oob = getelementptr inbounds i32, i32* %p, i64 5
      store i32 1, i32* %oob, align 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}

TEST_F(AECheckerTest, ConstantExprGlobalAccessIsOverflow) {
  const char *source = R"(
    @g = global [10 x i32] zeroinitializer, align 4

    define i32 @main() {
      store i32 1, i32* getelementptr inbounds ([10 x i32], [10 x i32]* @g, i64 1, i64 0), align 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}

TEST_F(AECheckerTest, HeapAllocUsesThirdSizeArgument) {
  const char *source = R"(
    declare i8* @HeapAlloc(i8*, i32, i64)

    define i32 @main() {
      %p = call i8* @HeapAlloc(i8* null, i32 0, i64 10)
      %oob = getelementptr inbounds i8, i8* %p, i64 10
      store i8 1, i8* %oob, align 1
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, InterproceduralOverflow) {
  const char *source = R"(
    define void @helper(i32* %arr, i32 %idx) {
      %idx64 = sext i32 %idx to i64
      %gep = getelementptr inbounds i32, i32* %arr, i64 %idx64
      store i32 42, i32* %gep
      ret void
    }
    
    define i32 @main() {
      %arr = alloca [10 x i32], align 4
      %arr_ptr = bitcast [10 x i32]* %arr to i32*
      call void @helper(i32* %arr_ptr, i32 15)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false);
  EXPECT_GT(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, InterproceduralStoreSideEffectsReachCaller) {
  const char *source = R"(
    define void @set_null(i32** %slot) {
    entry:
      store i32* null, i32** %slot
      ret void
    }

    define void @test_side_effect() {
    entry:
      %slot = alloca i32*
      %x = alloca i32
      store i32* %x, i32** %slot
      call void @set_null(i32** %slot)
      %p = load i32*, i32** %slot
      %v = load i32, i32* %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, StubSafeBufAccess) {
  const char *source = R"(
    declare void @SAFE_BUFACCESS(i8*, i32)
    
    define void @test_safe_stub() {
      %arr = alloca [10 x i8], align 1
      %arr_ptr = bitcast [10 x i8]* %arr to i8*
      call void @SAFE_BUFACCESS(i8* %arr_ptr, i32 5)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.pending_checkpoints, 0u);
}
TEST_F(AECheckerTest, StubUnsafeBufAccess) {
  const char *source = R"(
    declare void @UNSAFE_BUFACCESS(i8*, i32)
    
    define void @test_unsafe_stub() {
      %arr = alloca [10 x i8], align 1
      %arr_ptr = bitcast [10 x i8]* %arr to i8*
      call void @UNSAFE_BUFACCESS(i8* %arr_ptr, i32 15)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.pending_checkpoints, 0u);
}
TEST_F(AECheckerTest, AbstractStateVLASize) {
  AbstractState as;

  // Create a mock alloca instruction context
  LLVMContext ctx;
  Module M("test", ctx);
  FunctionType *FTy = FunctionType::get(Type::getVoidTy(ctx), false);
  Function *F = Function::Create(FTy, Function::ExternalLinkage, "test", M);
  BasicBlock *BB = BasicBlock::Create(ctx, "entry", F);

  // Create VLA alloca with non-constant size
  Type *Int32Ty = Type::getInt32Ty(ctx);
  // Use a parameter as size (simulating VLA)
  Argument *SizeArg = new Argument(Int32Ty, "size");
  AllocaInst *VLA = new AllocaInst(Int32Ty, 0, SizeArg, "vla", BB);

  // Set up abstract state with size tracking
  uint32_t sizeId = AbstractInterpretation::getValueIdStatic(SizeArg);
  as[sizeId] = AbstractValue(IntervalValue(50, 100));

  // Test improved VLA size computation
  uint32_t computedSize = as.getAllocaInstByteSize(VLA, as);

  // Should use upper bound from abstract state (100) * sizeof(i32) = 400
  // But clamped to MaxFieldLimit if needed
  EXPECT_GT(computedSize, 0);
  EXPECT_LE(computedSize, MaxFieldLimit);
}
TEST_F(AECheckerTest, NestedGEPOffsetAccumulation) {
  const char *source = R"(
    define void @test_nested_gep_offset() {
      %arr = alloca [100 x i32], align 4
      %gep1 = getelementptr inbounds [100 x i32], [100 x i32]* %arr, i64 0, i64 10
      %gep2 = getelementptr inbounds i32, i32* %gep1, i64 5
      store i32 42, i32* %gep2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, NestedGEPOverflowAfterOffsetBase) {
  const char *source = R"(
    define void @test_nested_gep_overflow() {
      %arr = alloca [100 x i32], align 4
      %gep1 = getelementptr inbounds [100 x i32], [100 x i32]* %arr, i64 0, i64 90
      %gep2 = getelementptr inbounds i32, i32* %gep1, i64 20
      store i32 42, i32* %gep2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, BoundaryOverflow) {
  const char *source = R"(
    define void @test_boundary() {
      %arr = alloca [10 x i32], align 4
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 10
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, MultipleDetectors) {
  const char *source = R"(
    define void @test_multiple_issues() {
      %arr = alloca [10 x i32], align 4
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      %gep1 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 15
      %gep2 = getelementptr inbounds i32, i32* %p, i64 5
      store i32 42, i32* %gep1
      store i32 42, i32* %gep2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_GT(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, UseAfterFreeDetection) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @test_uaf() {
      %p = call i8* @malloc(i64 16)
      call void @free(i8* %p)
      %q = getelementptr inbounds i8, i8* %p, i64 1
      store i8 1, i8* %q
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.uaf_bugs, 0u);
}
TEST_F(AECheckerTest, FreedPointerAlsoCountsAsUnsafeDeref) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @test_freed_deref() {
      %p = call i8* @malloc(i64 16)
      call void @free(i8* %p)
      %q = getelementptr inbounds i8, i8* %p, i64 1
      store i8 1, i8* %q
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.null_bugs, 0u);
  EXPECT_GT(result.uaf_bugs, 0u);
}
TEST_F(AECheckerTest, InvalidFreeDetection) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @test_double_free() {
      %p = call i8* @malloc(i64 32)
      call void @free(i8* %p)
      call void @free(i8* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.invalid_free_bugs, 0u);
}
TEST_F(AECheckerTest, InvalidFreeOfStackObject) {
  const char *source = R"(
    declare void @free(i8*)

    define void @test_free_stack() {
      %buf = alloca [8 x i8], align 1
      %ptr = bitcast [8 x i8]* %buf to i8*
      call void @free(i8* %ptr)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.invalid_free_bugs, 0u);
}
TEST_F(AECheckerTest, InvalidFreeOfGlobalObject) {
  const char *source = R"(
    @g = global i8 0
    declare void @free(i8*)

    define void @test_free_global() {
      call void @free(i8* @g)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.invalid_free_bugs, 0u);
}
TEST_F(AECheckerTest, FirstFreeIsNotInvalid) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @test_free_once() {
      %p = call i8* @malloc(i64 32)
      call void @free(i8* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.invalid_free_bugs, 0u);
}
TEST_F(AECheckerTest, FreeNullIsNotInvalid) {
  const char *source = R"(
    declare void @free(i8*)

    define void @test_free_null() {
      call void @free(i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.invalid_free_bugs, 0u);
}
TEST_F(AECheckerTest, GlobalPointerInitializer) {
  const char *source = R"(
    @g = global i32 0
    @p = global i32* @g

    define i32 @main() {
    entry:
      %q = load i32*, i32** @p
      store i32 1, i32* %q
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false);
  EXPECT_EQ(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, GlobalAggregateInitializerPreservesNullField) {
  const char *source = R"(
    @x = global i32 42
    @ptrs = global [2 x i32*] [i32* null, i32* @x]

    define i32 @main() {
    entry:
      %slot = getelementptr inbounds [2 x i32*], [2 x i32*]* @ptrs, i64 0, i64 0
      %p = load i32*, i32** %slot
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false);
  EXPECT_GT(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, GlobalConstantExprInitializerTracksSubobjectOffset) {
  const char *source = R"(
    @arr = global [4 x i32] zeroinitializer
    @p = global i32* getelementptr inbounds ([4 x i32], [4 x i32]* @arr, i64 0, i64 3)

    define i32 @main() {
    entry:
      %q = load i32*, i32** @p
      %bad = getelementptr inbounds i32, i32* %q, i64 1
      store i32 1, i32* %bad
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false);
  EXPECT_GT(result.overflow_bugs, 0u);
}
TEST_F(AECheckerTest, IndirectExternalMemcpyOverflow) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @test_indirect_memcpy() {
      %dst = alloca [4 x i8], align 1
      %src = alloca [8 x i8], align 1
      %dst_ptr = bitcast [4 x i8]* %dst to i8*
      %src_ptr = bitcast [8 x i8]* %src to i8*
      %fp = alloca void (i8*, i8*, i64, i1)*, align 8
      store void (i8*, i8*, i64, i1)* @llvm.memcpy.p0i8.p0i8.i64,
            void (i8*, i8*, i64, i1)** %fp
      %f = load void (i8*, i8*, i64, i1)*, void (i8*, i8*, i64, i1)** %fp
      call void %f(i8* %dst_ptr, i8* %src_ptr, i64 8, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, IndirectExternalMemcpyAndMemmoveJoinNoNullFalsePositive) {
  const char *source = R"(
    declare i1 @unknown()
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
    declare void @llvm.memmove.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @test_indirect_copy_join() {
      %dst = alloca [4 x i8], align 1
      %src = alloca [8 x i8], align 1
      %dst_ptr = bitcast [4 x i8]* %dst to i8*
      %src_ptr = bitcast [8 x i8]* %src to i8*
      %cond = call i1 @unknown()
      %fp = select i1 %cond,
                   void (i8*, i8*, i64, i1)* @llvm.memcpy.p0i8.p0i8.i64,
                   void (i8*, i8*, i64, i1)* @llvm.memmove.p0i8.p0i8.i64
      call void %fp(i8* %dst_ptr, i8* %src_ptr, i64 8, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, RecursiveSelfCallTopModeReturnsTop) {
  const char *source = R"(
    define i32 @demo(i32 %a) {
    entry:
      %cmp = icmp sge i32 %a, 10000
      br i1 %cmp, label %base, label %recur

    recur:
      %inc = add i32 %a, 1
      %res = call i32 @demo(i32 %inc)
      ret i32 %res

    base:
      ret i32 %a
    }

    define i32 @main() {
    entry:
      %res = call i32 @demo(i32 0)
      ret i32 %res
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result =
      runAE(module.get(), true, false, AbstractInterpretation::TOP, 3u);
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);

  IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
  EXPECT_TRUE(ret.isTop()) << ret.toString();
}
TEST_F(AECheckerTest, RecursiveSelfCallWidenOnlyKeepsLowerBound) {
  const char *source = R"(
    define i32 @demo(i32 %a) {
    entry:
      %cmp = icmp sge i32 %a, 10000
      br i1 %cmp, label %base, label %recur

    recur:
      %inc = add i32 %a, 1
      %res = call i32 @demo(i32 %inc)
      ret i32 %res

    base:
      ret i32 %a
    }

    define i32 @main() {
    entry:
      %res = call i32 @demo(i32 0)
      ret i32 %res
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result =
      runAE(module.get(), true, false, AbstractInterpretation::WIDEN_ONLY, 3u);
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);

  IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
  EXPECT_FALSE(ret.isBottom());
  EXPECT_EQ(ret.lb().getIntNumeral(), 10000) << ret.toString();
  EXPECT_TRUE(ret.ub().is_infinity()) << ret.toString();
}
TEST_F(AECheckerTest, RecursiveSelfCallWidenNarrowPreservesRecursiveSummary) {
  const char *source = R"(
    define i32 @demo(i32 %a) {
    entry:
      %cmp = icmp sge i32 %a, 10000
      br i1 %cmp, label %base, label %recur

    recur:
      %inc = add i32 %a, 1
      %res = call i32 @demo(i32 %inc)
      ret i32 %res

    base:
      ret i32 %a
    }

    define i32 @main() {
    entry:
      %res = call i32 @demo(i32 0)
      ret i32 %res
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, false,
                          AbstractInterpretation::WIDEN_NARROW, 3u);
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);

  IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
  EXPECT_EQ(ret.lb().getIntNumeral(), 10000) << ret.toString();
  EXPECT_TRUE(ret.ub().is_infinity()) << ret.toString();
}
TEST_F(AECheckerTest, MutualRecursionPropagatesEntryCallStateAcrossSCC) {
  const char *source = R"(
    define i32 @odd(i32 %n) {
    entry:
      %cmp = icmp sge i32 %n, 5
      br i1 %cmp, label %base, label %step

    step:
      %inc = add i32 %n, 1
      %res = call i32 @even(i32 %inc)
      ret i32 %res

    base:
      ret i32 %n
    }

    define i32 @even(i32 %n) {
    entry:
      %cmp = icmp sge i32 %n, 5
      br i1 %cmp, label %base, label %step

    step:
      %inc = add i32 %n, 1
      %res = call i32 @odd(i32 %inc)
      ret i32 %res

    base:
      ret i32 %n
    }

    define i32 @main() {
    entry:
      %res = call i32 @odd(i32 0)
      ret i32 %res
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, false,
                          AbstractInterpretation::WIDEN_NARROW, 3u);
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);

  IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
  EXPECT_EQ(ret.lb().getIntNumeral(), 5) << ret.toString();
  EXPECT_TRUE(ret.ub().is_infinity()) << ret.toString();
}
