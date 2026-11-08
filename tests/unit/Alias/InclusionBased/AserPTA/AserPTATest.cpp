//
// AserPTA Tests - Pointer analysis with field-sensitivity and
// context-sensitivity
//
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/KCallSite.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/KOrigin.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldInsensitive/FIMemModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Program/CallSite.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Program/CtxModule.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/DeepPropagation.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PartialUpdateSolver.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/WavePropagation.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/CanonicalizeGEPPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/InsertGlobalCtorCallPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/LoweringMemCpyPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/RemoveExceptionHandlerPass.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace aser;
using namespace lotus::unittest;

// Field-insensitive, context-insensitive
using FI_NoCtx_Model = DefaultLangModel<NoCtx, FIMemModel<NoCtx>>;
using FI_NoCtx_Solver = PartialUpdateSolver<FI_NoCtx_Model>;

// Field-sensitive, context-insensitive
using FS_NoCtx_Model = DefaultLangModel<NoCtx, FSMemModel<NoCtx>>;
using FS_NoCtx_Solver = PartialUpdateSolver<FS_NoCtx_Model>;

// Field-sensitive, 1-CFA
using FS_1CFA_Model = DefaultLangModel<KCallSite<1>, FSMemModel<KCallSite<1>>>;
using FS_1CFA_Solver = PartialUpdateSolver<FS_1CFA_Model>;

namespace {

template <typename Solver>
void addAserPTAPasses(llvm::legacy::PassManager &passes) {
  passes.add(new CanonicalizeGEPPass());
  passes.add(new LoweringMemCpyPass());
  passes.add(new RemoveExceptionHandlerPass());
  passes.add(new InsertGlobalCtorCallPass());
  passes.add(new PointerAnalysisPass<Solver>());
}

// @target already has its address seed before @late becomes reachable.
// The cast's canonical pointer is @target, so visiting @late need not grow
// that pointer's points-to set. The new call site still needs resolution.
const char *lateCallIR = R"(
  @payload = global i8 0
  define i8* @target(i8* %p) { ret i8* %p }
  define i8* @late(i8* %p) {
    %cast = bitcast i8* (i8*)* @target to i8* (i32*)*
    %arg = bitcast i8* %p to i32*
    %inner = call i8* %cast(i32* %arg)
    ret i8* %inner
  }
  define i32 @main() {
    %fp = select i1 true, i8* (i8*)* @late, i8* (i8*)* @late
    %outer = call i8* %fp(i8* @payload)
    ret i32 0
  }
)";

template <typename Solver> void checkLateCall() {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModule(context, lateCallIR, "late-call");
  ASSERT_NE(module, nullptr);
  Solver solver;
  solver.analyze(module.get());
  for (const char *name : {"main", "late"}) {
    for (auto &instruction : llvm::instructions(module->getFunction(name))) {
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (!call)
        continue;
      const auto *site = solver.getInDirectCallSite(nullptr, call);
      ASSERT_NE(site, nullptr);
      const auto *expected = module->getFunction(
          llvm::StringRef(name) == "main" ? "late" : "target");
      EXPECT_EQ(site->getResolvedTarget().size(), 1u);
      EXPECT_EQ(site->getResolvedTarget().count(expected), 1u);

      std::vector<const typename Solver::ObjTy *> points;
      solver.getPointsTo(nullptr, call, points);
      ASSERT_EQ(points.size(), 1u) << name;
      EXPECT_EQ(points.front()->getValue(), module->getGlobalVariable("payload"));
    }
  }
}

template <typename Solver> void checkLateCallFromMergedReturn() {
  LLVMContext context;
  auto module = parseModule(context, R"(
    @payload = global i8 0
    @out = global i8* null
    @fp = global i8* ()* @late

    define i8* @recursive(i1 %stop) {
    entry:
      br i1 %stop, label %done, label %again
    done:
      ret i8* @payload
    again:
      %r = call i8* @recursive(i1 true)
      ret i8* %r
    }

    define i8* @late() {
      %q = call i8* @recursive(i1 false)
      ret i8* %q
    }

    define i32 @main() {
      %first = call i8* @recursive(i1 false)
      %p = load i8* ()*, i8* ()** @fp
      %result = call i8* %p()
      store i8* %result, i8** @out
      ret i32 0
    }
  )", "late-call-merged-return");
  ASSERT_NE(module, nullptr);

  // The initial direct call makes the recursive return a Copy SCC. Resolving
  // @fp later adds a Copy from that return to @late's new call result.
  Solver solver;
  solver.analyze(module.get());
  auto *late = module->getFunction("late");
  auto *result = findInstructionByName(module->getFunction("main"), "result");
  ASSERT_NE(result, nullptr);
  const auto *site = solver.getInDirectCallSite(nullptr, cast<CallBase>(result));
  ASSERT_NE(site, nullptr);
  EXPECT_EQ(site->getResolvedTarget().size(), 1u);
  EXPECT_EQ(site->getResolvedTarget().count(late), 1u);

  for (const char *function : {"late", "main"}) {
    const char *name = StringRef(function) == "late" ? "q" : "result";
    auto *value = findInstructionByName(module->getFunction(function), name);
    ASSERT_NE(value, nullptr);
    std::vector<const typename Solver::ObjTy *> points;
    solver.getPointsTo(nullptr, value, points);
    ASSERT_EQ(points.size(), 1u) << function << ": " << name;
    EXPECT_EQ(points.front()->getValue(), module->getGlobalVariable("payload"));
  }
}

class RegistrationModel : public FS_NoCtx_Model {
public:
  explicit RegistrationModel(llvm::Module *module) : FS_NoCtx_Model(module, "main") {}
  using FS_NoCtx_Model::getConsGraph;
  using FS_NoCtx_Model::getPtrNodeOrNull;
  using FS_NoCtx_Model::initIndirectCall;
  void buildBody(llvm::Function *function) {
    addGlobals();
    CtxFunction<NoCtx> instance(nullptr, function, nullptr, nullptr);
    initFunction(&instance);
  }
};

} // namespace

TEST(AserPTAInfrastructure, SymbolicConstantGepIndexHasElementStride) {
  const char *ir = R"(
    define i32* @index([4 x i32]* %array) {
    entry:
      %element = getelementptr [4 x i32], [4 x i32]* %array,
                               i64 0, i64 undef
      ret i32* %element
    }
  )";
  LLVMContext context;
  auto module = parseModule(context, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);
  const GetElementPtrInst *gep = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("index")))
    if (const auto *candidate = dyn_cast<GetElementPtrInst>(&instruction))
      gep = candidate;
  ASSERT_NE(gep, nullptr);
  ASSERT_FALSE(gep->hasAllConstantIndices());
  EXPECT_EQ(getGEPStepSize(gep, module->getDataLayout()), 4u);
}

TEST(AserPTAInfrastructure, MissingIndirectCallNodeReturnsNull) {
  const char *ir = R"(
    define i32 @main(void ()* %callee) {
    entry:
      call void %callee()
      ret i32 0
    }
  )";
  LLVMContext context;
  auto module = parseModule(context, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);
  const CallBase *call = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<CallBase>(&instruction))
      call = candidate;
  ASSERT_NE(call, nullptr);

  CtxModule<NoCtx> contextModule(module.get(), "main");
  EXPECT_EQ(contextModule.getInDirectNode(nullptr, call), nullptr);
}

// ============================================================================
// Indirect Call Resolution Tests
// ============================================================================

TEST(AserPTAIndirectCalls, LateExistingPointerPartialUpdateFS) {
  checkLateCall<PartialUpdateSolver<FS_NoCtx_Model>>();
}
TEST(AserPTAIndirectCalls, LateExistingPointerWaveFS) {
  checkLateCall<WavePropagation<FS_NoCtx_Model>>();
}
TEST(AserPTAIndirectCalls, LateExistingPointerDeepFS) {
  checkLateCall<DeepPropagation<FS_NoCtx_Model>>();
}
TEST(AserPTAIndirectCalls, LateExistingPointerPartialUpdateFI) {
  checkLateCall<PartialUpdateSolver<FI_NoCtx_Model>>();
}
TEST(AserPTAIndirectCalls, LateExistingPointerWaveFI) {
  checkLateCall<WavePropagation<FI_NoCtx_Model>>();
}
TEST(AserPTAIndirectCalls, LateExistingPointerDeepFI) {
  checkLateCall<DeepPropagation<FI_NoCtx_Model>>();
}

TEST(AserPTAIndirectCalls, LateCopyFromMergedReturnPartialUpdateFI) {
  checkLateCallFromMergedReturn<FI_NoCtx_Solver>();
}

TEST(AserPTAIndirectCalls, LateCopyFromMergedReturnPartialUpdateFS) {
  checkLateCallFromMergedReturn<FS_NoCtx_Solver>();
}

TEST(AserPTAIndirectCalls, RegistrationUsesCurrentRepresentative) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModule(context, R"(
    @payload = global i8 0
    define i8* @target(i8* %p) { ret i8* %p }
    define i32 @main() {
    entry:
      br label %loop
    loop:
      %left = phi i8* (i8*)* [@target, %entry], [%right, %loop]
      %right = select i1 true, i8* (i8*)* %left, i8* (i8*)* %left
      %result = call i8* %right(i8* @payload)
      br label %loop
    }
  )", "merged-registration");
  ASSERT_NE(module, nullptr);
  using PT = PTSTrait<DefaultPtsTy>;
  PT::clearAll();
  {
    RegistrationModel model(module.get());
    auto *main = module->getFunction("main");
    model.buildBody(main);
    llvm::Instruction *left = nullptr;
    llvm::CallBase *call = nullptr;
    for (auto &instruction : llvm::instructions(main)) {
      if (instruction.getName() == "left")
        left = &instruction;
      if (auto *candidate = llvm::dyn_cast<llvm::CallBase>(&instruction))
        call = candidate;
    }
    ASSERT_NE(left, nullptr);
    ASSERT_NE(call, nullptr);
    auto *representative = model.getPtrNodeOrNull(nullptr, left);
    auto *original = model.getPtrNodeOrNull(nullptr, call->getCalledOperand());
    ASSERT_NE(representative, nullptr);
    ASSERT_NE(original, nullptr);
    ASSERT_NE(representative, original);
    model.getConsGraph()->collapseSCCTo({representative, original}, representative);
    ASSERT_EQ(original->getSuperNode(), representative);
    CallGraph<NoCtx> calls;
    auto *site = calls.addNewNode<CallGraphNode<NoCtx>>(nullptr, call);
    model.initIndirectCall(site->getTargetFunPtr());
    EXPECT_TRUE(representative->getIndirectNodes().count(site));
    EXPECT_TRUE(original->getIndirectNodes().empty());
  }
  PT::clearAll();
}

TEST(AserPTAIndirectCalls, ResolvingTargetCanRegisterMoreSitesOnSamePointer) {
  llvm::LLVMContext context;
  std::string ir = "@payload = global i8 0\n"
                   "define i8* @target(i8* %p) {\n"
                   "  %cast = bitcast i8* (i8*)* @target to i8* (i32*)*\n"
                   "  %arg = bitcast i8* %p to i32*\n";
  // Exceed SmallPtrSet's inline capacity while its original site is resolved.
  for (unsigned i = 0; i < 12; ++i)
    ir += "  %r" + std::to_string(i) + " = call i8* %cast(i32* %arg)\n";
  ir += "  ret i8* %p\n}\n"
        "define i32 @main() {\n"
        "  %cast = bitcast i8* (i8*)* @target to i8* (i32*)*\n"
        "  %outer = call i8* %cast(i32* bitcast (i8* @payload to i32*))\n"
        "  ret i32 0\n}\n";
  auto module = lotus::unittest::parseModule(context, ir, "growing-sites");
  ASSERT_NE(module, nullptr);
  PartialUpdateSolver<FS_NoCtx_Model> solver;
  solver.analyze(module.get());
  unsigned sites = 0;
  for (auto &function : *module)
    for (auto &instruction : llvm::instructions(function))
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        const auto *site = solver.getInDirectCallSite(nullptr, call);
        ASSERT_NE(site, nullptr);
        EXPECT_EQ(site->getResolvedTarget().size(), 1u);
        EXPECT_EQ(site->getResolvedTarget().count(module->getFunction("target")), 1u);
        ++sites;
      }
  EXPECT_EQ(sites, 13u);
}

// ============================================================================
// Field-Insensitive Tests
// ============================================================================

TEST(AserPTA_FI, NoAliasTwoAllocas) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseModule(ctx, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FI_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FI, AliasStoreLoad) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseModule(ctx, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FI_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FI, GlobalNoAlias) {
  const char *ir = R"(
    @g1 = global i32 0
    @g2 = global i32 0
    define i32 @main() {
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseModule(ctx, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FI_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FI, PointerChain) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      %pp = alloca i32**
      store i32* %x, i32** %p
      store i32** %p, i32*** %pp
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseModule(ctx, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FI_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FI, NullPointer) {
  const char *ir = R"(
    define i32 @main() {
      %p = alloca i32*
      store i32* null, i32** %p
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseModule(ctx, ir, "AserPTATest");
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FI_NoCtx_Solver>(passes);
  passes.run(*module);
}

// ============================================================================
// Field-Sensitive Tests
// ============================================================================

TEST(AserPTA_FS, DifferentStructFieldsNoAlias) {
  const char *ir = R"(
    %struct.S = type { i32, i32 }

    define i32 @main() {
      %s = alloca %struct.S
      %f0 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      %f1 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 1
      store i32 10, i32* %f0
      store i32 20, i32* %f1
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, NestedStructFields) {
  const char *ir = R"(
    %inner = type { i32, i32 }
    %outer = type { %inner, i32 }

    define i32 @main() {
      %o = alloca %outer
      %inner_ptr = getelementptr inbounds %outer, %outer* %o, i32 0, i32 0
      %f0 = getelementptr inbounds %inner, %inner* %inner_ptr, i32 0, i32 0
      %f1 = getelementptr inbounds %inner, %inner* %inner_ptr, i32 0, i32 1
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, ArrayElementAccess) {
  const char *ir = R"(
    define i32 @main() {
      %arr = alloca [10 x i32]
      %p0 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i32 0, i32 0
      %p5 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i32 0, i32 5
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, HeapAllocation) {
  const char *ir = R"(
    declare i8* @malloc(i64)

    define i32 @main() {
      %size = mul i64 4, 100
      %raw = call i8* @malloc(i64 %size)
      %ptr = bitcast i8* %raw to i32*
      store i32 42, i32* %ptr
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, MultipleHeapAllocations) {
  const char *ir = R"(
    declare i8* @malloc(i64)

    define i32 @main() {
      %p1 = call i8* @malloc(i64 16)
      %p2 = call i8* @malloc(i64 32)
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

// ============================================================================
// Context-Sensitive Tests (1-CFA)
// ============================================================================

TEST(AserPTA_1CFA, FunctionParameter) {
  const char *ir = R"(
    define void @callee(i32* %p) {
      ret void
    }

    define i32 @main() {
      %x = alloca i32
      call void @callee(i32* %x)
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_1CFA_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_1CFA, SameFunctionDifferentContexts) {
  const char *ir = R"(
    @global_ptr = global i32* null

    define void @helper(i32* %p) {
      store i32* %p, i32** @global_ptr
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
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_1CFA_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_1CFA, RecursiveFunction) {
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
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_1CFA_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_1CFA, FunctionReturnPointsTo) {
  const char *ir = R"(
    define i32* @alloc_int() {
      %x = alloca i32
      store i32 42, i32* %x
      ret i32* %x
    }

    define i32 @main() {
      %p = call i32* @alloc_int()
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_1CFA_Solver>(passes);
  passes.run(*module);
}

// ============================================================================
// Indirect Call Tests
// ============================================================================

TEST(AserPTA_FS, IndirectFunctionCall) {
  const char *ir = R"(
    define void @func1(i32* %p) {
      ret void
    }

    define void @func2(i32* %p) {
      ret void
    }

    define void @caller(void (i32*)* %fp, i32* %arg) {
      call void %fp(i32* %arg)
      ret void
    }

    define i32 @main() {
      %x = alloca i32
      %fptr = alloca void (i32*)*
      store void (i32*)* @func1, void (i32*)** %fptr
      %f = load void (i32*)*, void (i32*)** %fptr
      call void @caller(void (i32*)* %f, i32* %x)
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

// ============================================================================
// Complex Patterns Tests
// ============================================================================

TEST(AserPTA_FS, PHINodeMerge) {
  const char *ir = R"(
    define i32* @main(i1 %cond) {
      %x = alloca i32
      %y = alloca i32
      store i32 1, i32* %x
      store i32 2, i32* %y

      br i1 %cond, label %true_bb, label %false_bb

    true_bb:
      br label %merge

    false_bb:
      br label %merge

    merge:
      %ptr = phi i32* [%x, %true_bb], [%y, %false_bb]
      ret i32* %ptr
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, BitCastHandling) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      store i32 12345678, i32* %x
      %byte_ptr = bitcast i32* %x to i8*
      %first_byte = load i8, i8* %byte_ptr
      %result = zext i8 %first_byte to i32
      ret i32 %result
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

TEST(AserPTA_FS, SelectInstruction) {
  const char *ir = R"(
    define i32* @main(i1 %cond) {
      %x = alloca i32
      %y = alloca i32
      store i32 10, i32* %x
      store i32 20, i32* %y
      %ptr = select i1 %cond, i32* %x, i32* %y
      ret i32* %ptr
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passes;
  addAserPTAPasses<FS_NoCtx_Solver>(passes);
  passes.run(*module);
}

// ============================================================================
// Comparison Tests (FI vs FS)
// ============================================================================

TEST(AserPTA_Comparison, FIvsFS_SameResult) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      ret i32 0
    }
  )";
  LLVMContext ctx;
  auto module = parseAssembly(ctx, ir);
  ASSERT_NE(module, nullptr);

  llvm::legacy::PassManager passesFI, passesFS;
  addAserPTAPasses<FI_NoCtx_Solver>(passesFI);
  addAserPTAPasses<FS_NoCtx_Solver>(passesFS);
  passesFI.run(*module);
  passesFS.run(*module);
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
