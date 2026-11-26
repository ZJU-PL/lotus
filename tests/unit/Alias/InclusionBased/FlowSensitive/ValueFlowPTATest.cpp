#include "Alias/InclusionBased/FlowSensitive/ValueFlow/ValueFlowPTA.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>

using lotus::alias::ValueFlowPTA;
using namespace llvm;
namespace {
const Value *value(const Module &module, const char *name) {
  if (const Function *main = module.getFunction("main")) {
    for (const BasicBlock &block : *main)
      for (const Instruction &instruction : block)
        if (instruction.getName() == name)
          return &instruction;
  }
  for (const Function &function : module)
    for (const BasicBlock &block : function)
      for (const Instruction &instruction : block)
        if (instruction.getName() == name)
          return &instruction;
  throw std::runtime_error(std::string("missing value: ") + name);
}
void expect(const ValueFlowPTA &analysis, const Module &module,
            const char *name, std::initializer_list<const char *> objects) {
  ValueFlowPTA::PointsToSet wanted;
  for (const char *object : objects) {
    if (std::string(object) == "null")
      wanted.insert(analysis.getNullObjectId());
    else if (std::string(object) == "unknown")
      wanted.insert(analysis.getUnknownObjectId());
    else {
      const Value *allocation = module.getNamedValue(object);
      if (!allocation)
        allocation = value(module, object);
      wanted.insert(analysis.getObjectId(allocation));
    }
  }
  if (analysis.getPointsTo(value(module, name)) != wanted)
    throw std::runtime_error(std::string("incorrect points-to set: ") + name);
}
struct AdapterCase {
  const char *name;
  const char *ir;
  std::function<void(const ValueFlowPTA &, const Module &)> check;
};
} // namespace

TEST(ValueFlowPTATest, LLVMAdapterRegressions) {
  const std::vector<AdapterCase> tests{
      {"strong overwrite", R"IR(
      @A = global i8 0
      @B = global i8 0
      define i8* @main() {
        %slot = alloca i8*
        store i8* @A, i8** %slot
        %before = load i8*, i8** %slot
        store i8* @B, i8** %slot
        %after = load i8*, i8** %slot
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "before", {"A"});
         expect(a, m, "after", {"B"});
         if (a.mayAlias(value(m, "before"), value(m, "after")))
           throw std::runtime_error("disjoint results alias");
       }},
      {"null overwrite", R"IR(
      @A = global i8 0
      define i8* @main() {
        %slot = alloca i8*
        store i8* @A, i8** %slot
        store i8* null, i8** %slot
        %after = load i8*, i8** %slot
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "after", {"null"});
       }},
      {"direct swap", R"IR(
      @A = global i8 0
      @B = global i8 0
      define void @swap(i8** %p, i8** %q) {
        %oldp = load i8*, i8** %p
        %oldq = load i8*, i8** %q
        store i8* %oldq, i8** %p
        store i8* %oldp, i8** %q
        ret void
      }
      define i8* @main() {
        %x = alloca i8*
        %y = alloca i8*
        store i8* @A, i8** %x
        store i8* @B, i8** %y
        %before = load i8*, i8** %x
        call void @swap(i8** %x, i8** %y)
        %after = load i8*, i8** %x
        %other = load i8*, i8** %y
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "before", {"A"});
         expect(a, m, "after", {"B"});
         expect(a, m, "other", {"A"});
       }},
      {"escape ordered indirect overwrite", R"IR(
      @A = global i8 0
      @B = global i8 0
      define i8* @main() {
        %slot = alloca i8*
        %holder = alloca i8**
        store i8* @A, i8** %slot
        store i8** %slot, i8*** %holder
        %alias = load i8**, i8*** %holder
        store i8* @B, i8** %alias
        %after = load i8*, i8** %slot
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "after", {"B"});
         if (a.getStatistics().solver.usedWeakFallback)
           throw std::runtime_error("acyclic program used weak fallback");
       }},
      {"loaded function pointer", R"IR(
      @A = global i8 0
      @B = global i8 0
      @fp = global void (i8**)* @set
      define void @set(i8** %p) {
        store i8* @B, i8** %p
        ret void
      }
      define i8* @main() {
        %slot = alloca i8*
        store i8* @A, i8** %slot
        %callee = load void (i8**)*, void (i8**)** @fp
        call void %callee(i8** %slot)
        %after = load i8*, i8** %slot
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "after", {"B"});
         if (a.getStatistics().resolvedIndirectTargets != 1)
           throw std::runtime_error("function pointer not resolved");
       }},
      {"realloc may return its original object", R"IR(
      @A = global i8 0
      declare i8* @realloc(i8*, i64)
      define i8* @main() {
        %resized = call i8* @realloc(i8* @A, i64 8)
        ret i8* %resized
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "resized", {"A", "resized", "null"});
       }},
      {"weak select", R"IR(
      @A = global i8 0
      @B = global i8 0
      define i8* @main(i1 %condition) {
        %x = alloca i8*
        %y = alloca i8*
        store i8* @A, i8** %x
        store i8* @A, i8** %y
        %which = select i1 %condition, i8** %x, i8** %y
        store i8* @B, i8** %which
        %after = load i8*, i8** %x
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "after", {"A", "B"});
       }},
      {"external clobber", R"IR(
      @A = global i8 0
      declare void @mutate(i8**)
      define i8* @main() {
        %slot = alloca i8*
        store i8* @A, i8** %slot
        call void @mutate(i8** %slot)
        %after = load i8*, i8** %slot
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "after", {"A", "unknown"});
       }},
      {"readonly call", R"IR(
      @A = global i8 0
      declare void @observe(i8**) readonly
      define i8* @main() {
        %slot = alloca i8*
        store i8* @A, i8** %slot
        call void @observe(i8** %slot)
        %after = load i8*, i8** %slot
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "after", {"A"});
       }},
      {"monolithic aggregate", R"IR(
      @A = global i8 0
      @B = global i8 0
      @pair = global { i8*, i8* } { i8* @A, i8* @B }
      define i8* @main() {
        %field = getelementptr { i8*, i8* }, { i8*, i8* }* @pair, i32 0, i32 0
        %after = load i8*, i8** %field
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "after", {"A", "B"});
       }},
      {"byval does not overwrite caller", R"IR(
      @A = global i8 0
      @B = global i8 0
      define void @change(i8** byval(i8*) %copy) {
        store i8* @B, i8** %copy
        ret void
      }
      define i8* @main() {
        %slot = alloca i8*
        store i8* @A, i8** %slot
        call void @change(i8** byval(i8*) %slot)
        %after = load i8*, i8** %slot
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "after", {"A"});
       }},
      {"zero-byte memcpy", R"IR(
      @A = global i8 0
      declare void @llvm.memcpy.p0i8.p0i8.i64(i8* noalias nocapture writeonly, i8* noalias nocapture readonly, i64, i1 immarg)
      define i8* @main() {
        %slot = alloca i8*
        %src = alloca i8*
        store i8* @A, i8** %slot
        %dstbytes = bitcast i8** %slot to i8*
        %srcbytes = bitcast i8** %src to i8*
        call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dstbytes, i8* %srcbytes, i64 0, i1 false)
        %after = load i8*, i8** %slot
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "after", {"A"});
       }},
      {"partial-byte memcpy", R"IR(
      @A = global i8 0
      declare void @llvm.memcpy.p0i8.p0i8.i64(i8* noalias nocapture writeonly, i8* noalias nocapture readonly, i64, i1 immarg)
      define i8* @main() {
        %slot = alloca i8*
        %src = alloca i8*
        store i8* @A, i8** %slot
        %dstbytes = bitcast i8** %slot to i8*
        %srcbytes = bitcast i8** %src to i8*
        call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dstbytes, i8* %srcbytes, i64 1, i1 false)
        %after = load i8*, i8** %slot
        ret i8* %after
      }
    )IR",
       [](const ValueFlowPTA &a, const Module &m) {
         expect(a, m, "after", {"A", "unknown"});
       }}};
  for (const AdapterCase &test : tests) {
    SCOPED_TRACE(test.name);
    LLVMContext context;
    SMDiagnostic diagnostic;
    auto module = parseAssemblyString(test.ir, diagnostic, context);
    if (!module)
      diagnostic.print("ValueFlowPTATest", errs());
    ASSERT_NE(module, nullptr);
    ASSERT_FALSE(verifyModule(*module, &errs()));
    ValueFlowPTA analysis(*module);
    ASSERT_NO_THROW(analysis.analyze());
    ASSERT_NO_THROW(test.check(analysis, *module));
    ASSERT_NO_THROW(analysis.analyze());
    ASSERT_NO_THROW(test.check(analysis, *module));
  }
}

TEST(ValueFlowPTATest, DirectCallClosureNeedsOneSolveAndSkipsDeadCallers) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    declare void @opaque()
    define i8* @leaf() { ret i8* @A }
    define i8* @middle() { %r = call i8* @leaf() ret i8* %r }
    define i8* @main() { %r = call i8* @middle() ret i8* %r }
    define void @dead() { call void @opaque() ret void }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "r", {"A"});
  EXPECT_EQ(analysis.getStatistics().callGraphIterations, 1u);
  EXPECT_EQ(analysis.getStatistics().conservativeExternalCalls, 0u);
}

TEST(ValueFlowPTATest, NoCallbackAttributeSuppressesOnlyCallbackPaths) {
  for (bool noCallback : {false, true}) {
    LLVMContext context;
    SMDiagnostic diagnostic;
    std::string ir = R"(
      @A = global i8 0
      @B = global i8 0
      @slot = global i8* @A
      define void @callback() { store i8* @B, i8** @slot ret void }
      define i8* @main() {
        call void @opaque(void ()* @callback)
        %r = load i8*, i8** @slot
        ret i8* %r
      }
      declare void @opaque(void ()*)
    )";
    if (noCallback)
      ir += " nocallback";
    auto module = parseAssemblyString(ir, diagnostic, context);
    ASSERT_NE(module, nullptr);
    ASSERT_FALSE(verifyModule(*module, &errs()));
    ValueFlowPTA analysis(*module);
    analysis.analyze();
    const auto &result = analysis.getPointsTo(value(*module, "r"));
    EXPECT_EQ(result.count(analysis.getObjectId(module->getNamedValue("B"))),
              noCallback ? 0u : 1u);
    EXPECT_TRUE(result.count(analysis.getUnknownObjectId()));
  }
}

TEST(ValueFlowPTATest, SharedCalleePreservesLocalFlowSensitivity) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    define void @noop() { ret void }
    define i8* @main() {
      %x = alloca i8*
      store i8* @A, i8** %x
      call void @noop()
      %r1 = load i8*, i8** %x
      store i8* @B, i8** %x
      call void @noop()
      %r2 = load i8*, i8** %x
      ret i8* %r2
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "r1", {"A"});
  expect(analysis, *module, "r2", {"B"});
  EXPECT_FALSE(analysis.mayAlias(value(*module, "r1"), value(*module, "r2")));
  EXPECT_FALSE(analysis.getStatistics().solver.usedWeakFallback);
}

TEST(ValueFlowPTATest, PaperFigure1ExactLLVMIR) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    define void @swap(i8** %p, i8** %q) {
      %s1 = load i8*, i8** %p
      %s2 = load i8*, i8** %q
      store i8* %s2, i8** %p
      store i8* %s1, i8** %q
      ret void
    }
    define i8* @main(i1 %cond) {
    bb0:
      %t1 = alloca i8*
      %t2 = alloca i8*
      store i8* @A, i8** %t1
      store i8* @B, i8** %t2
      br i1 %cond, label %bb1, label %bb2
    bb1:
      %a1 = load i8*, i8** %t1
      br label %bb3
    bb2:
      call void @swap(i8** %t1, i8** %t2)
      %a2 = load i8*, i8** %t1
      br label %bb3
    bb3:
      %a3 = load i8*, i8** %t1
      ret i8* %a3
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "a1", {"A"});
  expect(analysis, *module, "a2", {"B"});
  expect(analysis, *module, "a3", {"A", "B"});
  EXPECT_FALSE(analysis.getStatistics().solver.usedWeakFallback);
  EXPECT_GT(analysis.getStatistics().solver.interproceduralAuxEdges, 0u);
}

TEST(ValueFlowPTATest, HeapAndAggregatesAvoidUnknownPoisoning) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    %struct.Mixed = type { i32, i8* }
    @A = global i8 0
    @B = global i8 0
    @mixedGlobal = global %struct.Mixed { i32 42, i8* @A }
    declare i8* @malloc(i64)
    declare i8* @calloc(i64, i64)
    declare i32 @printf(i8*, ...)
    define i8* @main() {
      %h1 = call i8* @malloc(i64 8)
      %h1p = bitcast i8* %h1 to i8**
      store i8* @A, i8** %h1p
      %h2 = call i8* @malloc(i64 8)
      %h2p = bitcast i8* %h2 to i8**
      store i8* @B, i8** %h2p
      %r1 = load i8*, i8** %h1p
      %r2 = load i8*, i8** %h2p

      %cz = call i8* @calloc(i64 1, i64 8)
      %czp = bitcast i8* %cz to i8**
      %rc = load i8*, i8** %czp

      %st = alloca %struct.Mixed
      %tag = getelementptr %struct.Mixed, %struct.Mixed* %st, i32 0, i32 0
      %fld = getelementptr %struct.Mixed, %struct.Mixed* %st, i32 0, i32 1
      store i32 7, i32* %tag
      store i8* @A, i8** %fld
      %rs = load i8*, i8** %fld

      %gfld = getelementptr %struct.Mixed, %struct.Mixed* @mixedGlobal, i32 0, i32 1
      %rg = load i8*, i8** %gfld

      %pcall = call i32 (i8*, ...) @printf(i8* @A)
      %afterPrintf = load i8*, i8** %fld
      ret i8* %afterPrintf
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "r1", {"A"});
  expect(analysis, *module, "r2", {"B"});
  EXPECT_FALSE(analysis.mayAlias(value(*module, "r1"), value(*module, "r2")));
  expect(analysis, *module, "rc", {"null"});
  expect(analysis, *module, "rs", {"A"});
  expect(analysis, *module, "rg", {"A"});
  expect(analysis, *module, "afterPrintf", {"A"});
  EXPECT_EQ(analysis.getStatistics().conservativeExternalCalls, 0u);
  EXPECT_FALSE(analysis.getStatistics().solver.usedWeakFallback);
}

TEST(ValueFlowPTATest, GlobalCtorsAndDtorsLoweredInPriorityOrder) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    @C = global i8 0
    @g = global i8* null
    @llvm.global_ctors = appending global [2 x { i32, void ()*, i8* }] [
      { i32, void ()*, i8* } { i32 200, void ()* @ctor_late, i8* null },
      { i32, void ()*, i8* } { i32 100, void ()* @ctor_early, i8* null }
    ]
    @llvm.global_dtors = appending global [1 x { i32, void ()*, i8* }] [
      { i32, void ()*, i8* } { i32 100, void ()* @dtor_fn, i8* null }
    ]
    define void @ctor_early() {
      store i8* @A, i8** @g
      ret void
    }
    define void @ctor_late() {
      store i8* @B, i8** @g
      ret void
    }
    define void @dtor_fn() {
      store i8* @C, i8** @g
      ret void
    }
    define i8* @main() {
      %r = load i8*, i8** @g
      ret i8* %r
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  ASSERT_NO_THROW(analysis.analyze());
  expect(analysis, *module, "r", {"B"});
  EXPECT_EQ(analysis.getStatistics().globalCtors, 2u);
  EXPECT_EQ(analysis.getStatistics().globalDtors, 1u);
  EXPECT_FALSE(analysis.getStatistics().solver.usedWeakFallback);
}

TEST(ValueFlowPTATest, VectorGEPDoesNotCrash) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    define void @main(<2 x i8*> %v) {
      %g = getelementptr i8, <2 x i8*> %v, <2 x i64> zeroinitializer
      ret void
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  ASSERT_NO_THROW(analysis.analyze());
}

TEST(ValueFlowPTATest, FullMemcpyAndZeroMemsetAndByValAndPosixMemalign) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1 immarg)
    declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1 immarg)
    declare i32 @posix_memalign(i8**, i64, i64)
    define i8* @readByVal(i8** byval(i8*) %copy) {
      %in = load i8*, i8** %copy
      store i8* @B, i8** %copy
      ret i8* %in
    }
    define i8* @main() {
      %src = alloca i8*
      %dst = alloca i8*
      store i8* @A, i8** %src
      %srcb = bitcast i8** %src to i8*
      %dstb = bitcast i8** %dst to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dstb, i8* %srcb, i64 8, i1 false)
      %copied = load i8*, i8** %dst

      %zslot = alloca i8*
      store i8* @A, i8** %zslot
      %zb = bitcast i8** %zslot to i8*
      call void @llvm.memset.p0i8.i64(i8* %zb, i8 0, i64 8, i1 false)
      %zeroed = load i8*, i8** %zslot

      %fromByVal = call i8* @readByVal(i8** byval(i8*) %src)
      %afterByVal = load i8*, i8** %src

      %outPtr = alloca i8*
      store i8* null, i8** %outPtr
      %pm = call i32 @posix_memalign(i8** %outPtr, i64 16, i64 64)
      %aligned = load i8*, i8** %outPtr
      ret i8* %copied
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "copied", {"A"});
  expect(analysis, *module, "zeroed", {"A", "null"});
  expect(analysis, *module, "fromByVal", {"A"});
  expect(analysis, *module, "afterByVal", {"A"});
  expect(analysis, *module, "aligned", {"null", "pm"});
  EXPECT_FALSE(analysis.getStatistics().solver.usedWeakFallback);
}

TEST(ValueFlowPTATest, UnknownIndirectCallFiltersAddressTakenAndKeepsMainSingleton) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    @C = global i8 0
    @g = global i8* @A
    define void @taken() {
      store i8* @B, i8** @g
      ret void
    }
    define void @not_taken() {
      store i8* @C, i8** @g
      ret void
    }
    define i8* @main(void ()* %unknown_fp) {
      %keep = alloca void ()*
      store void ()* @taken, void ()** %keep
      %local = alloca i8*
      store i8* @A, i8** %local
      store i8* @B, i8** %local
      %localVal = load i8*, i8** %local
      call void %unknown_fp()
      %gVal = load i8*, i8** @g
      ret i8* %gVal
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  // main is NOT address-taken, so %local stays a strong singleton!
  expect(analysis, *module, "localVal", {"B"});
  // @taken IS address-taken, @not_taken is NOT!
  const auto &gPts = analysis.getPointsTo(value(*module, "gVal"));
  EXPECT_TRUE(gPts.count(analysis.getObjectId(module->getNamedValue("B"))));
  EXPECT_FALSE(gPts.count(analysis.getObjectId(module->getNamedValue("C"))));
}

TEST(ValueFlowPTATest, IndirectCallMutatingGlobalAndVFGInspectionAPI) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    @g = global i8* @A
    @fp = global void ()* @writer
    define void @writer() {
      store i8* @B, i8** @g
      ret void
    }
    define i8* @main() {
      %f = load void ()*, void ()** @fp
      call void %f()
      %r = load i8*, i8** @g
      ret i8* %r
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "r", {"B"});
  EXPECT_EQ(analysis.getStatistics().callGraphIterations, 1u);
  EXPECT_EQ(analysis.getStatistics().resolvedIndirectTargets, 1u);
  EXPECT_FALSE(analysis.getStatistics().solver.usedWeakFallback);

  // Verify VFG inspection APIs on ValueFlowPTA.
  const auto objB = analysis.getObjectId(module->getNamedValue("B"));
  const auto storesOfB = analysis.getPointedToByStores(objB);
  EXPECT_EQ(storesOfB.size(), 1u);
  const StoreInst *storeB = *storesOfB.begin();
  const auto storeNode = analysis.getStoreNode(storeB);
  EXPECT_NE(storeNode, 0u);
  EXPECT_EQ(analysis.getNodeValue(storeNode), storeB);
  EXPECT_FALSE(analysis.getVFGIndirectSuccessors(storeNode).empty());
  const auto nodeB = analysis.getValueNode(module->getNamedValue("B"));
  EXPECT_TRUE(analysis.getVFGSuccessors(nodeB).count(storeNode));
  EXPECT_FALSE(analysis.getVFGIndirectSuccessors(nodeB).count(storeNode));
  EXPECT_GT(analysis.getSolver().vfgNodeCount(), analysis.getProgram().nodeCount());
}

TEST(ValueFlowPTATest, IndirectAllocatorAndReallocContentTransfer) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @allocFp = global i8* (i64)* @malloc
    declare i8* @malloc(i64)
    declare i8* @realloc(i8*, i64)
    define i8* @main() {
      %fn = load i8* (i64)*, i8* (i64)** @allocFp
      %buf = call i8* %fn(i64 8)
      %bufp = bitcast i8* %buf to i8**
      store i8* @A, i8** %bufp
      %grown = call i8* @realloc(i8* %buf, i64 16)
      %grownp = bitcast i8* %grown to i8**
      %loaded = load i8*, i8** %grownp
      ret i8* %loaded
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "buf", {"buf", "null"});
  expect(analysis, *module, "loaded", {"A"});
  EXPECT_EQ(analysis.getStatistics().conservativeExternalCalls, 0u);
  EXPECT_FALSE(analysis.getStatistics().solver.usedWeakFallback);
}

TEST(ValueFlowPTATest, IndirectCallThroughWrapperStronglyMutatesGlobal) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    @g = global i8* @A
    @fp = global void ()* @writer
    define void @writer() {
      store i8* @B, i8** @g
      ret void
    }
    define void @wrapper(void ()* %cb) {
      call void %cb()
      ret void
    }
    define i8* @main() {
      %f = load void ()*, void ()** @fp
      call void @wrapper(void ()* %f)
      %r = load i8*, i8** @g
      ret i8* %r
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "r", {"B"});
  EXPECT_EQ(analysis.getStatistics().callGraphIterations, 1u);
  EXPECT_FALSE(analysis.getStatistics().solver.usedWeakFallback);
}

TEST(ValueFlowPTATest, IndirectExternalAndByValCallsPreserveIntraBlockOrder) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    @pmFp = global i32 (i8**, i64, i64)* @posix_memalign
    @byvalFp = global i8* (i8**)* @readByVal
    declare i32 @posix_memalign(i8**, i64, i64)
    define i8* @readByVal(i8** byval(i8*) %copy) {
      %in = load i8*, i8** %copy
      ret i8* %in
    }
    define i8* @main() {
      ; Indirect posix_memalign followed immediately by load in SAME block:
      %out = alloca i8*
      store i8* null, i8** %out
      %f1 = load i32 (i8**, i64, i64)*, i32 (i8**, i64, i64)** @pmFp
      %pmCall = call i32 %f1(i8** %out, i64 16, i64 64)
      %afterPm = load i8*, i8** %out

      ; Indirect byval call followed immediately by overwrite in SAME block:
      %slot = alloca i8*
      store i8* @A, i8** %slot
      %f2 = load i8* (i8**)*, i8* (i8**)** @byvalFp
      %fromByVal = call i8* %f2(i8** byval(i8*) %slot)
      store i8* @B, i8** %slot
      %afterOverwrite = load i8*, i8** %slot
      ret i8* %fromByVal
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  // Same-block load after indirect posix_memalign MUST see the allocation!
  expect(analysis, *module, "afterPm", {"null", "pmCall"});
  // Indirect byval copy MUST run before call (sees A, NOT post-call B)!
  expect(analysis, *module, "fromByVal", {"A"});
  expect(analysis, *module, "afterOverwrite", {"B"});
}

TEST(ValueFlowPTATest, UnknownIndirectCallPreservesIntraBlockOrderAndCallbacks) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @C = global i8 0
    @cbObserved = global i8* null
    define void @cb(i8* %arg) {
      store i8* %arg, i8** @cbObserved
      ret void
    }
    define i8* @main(void (i8*)* %unknownFp) {
      %keepCb = alloca void (i8*)*
      store void (i8*)* @cb, void (i8*)** %keepCb

      ; Indirect unknown call followed immediately by load in SAME block:
      %gslot = alloca i8*
      store i8* @C, i8** %gslot
      call void %unknownFp(i8* @A)
      %afterUnknown = load i8*, i8** %gslot
      %cbVal = load i8*, i8** @cbObserved
      ret i8* %afterUnknown
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  // Same-block load after indirect unknown call MUST see unknown clobber!
  EXPECT_TRUE(analysis.getPointsTo(value(*module, "afterUnknown"))
                  .count(analysis.getUnknownObjectId()));
  // Callback invoked via unknown indirect call MUST receive unknown argument
  // (in addition to @A from direct indirect-target resolution of @cb)!
  EXPECT_TRUE(analysis.getPointsTo(value(*module, "cbVal"))
                  .count(analysis.getUnknownObjectId()));
}

TEST(ValueFlowPTATest, MixedDefinedAndExternalIndirectCallDoesNotUnsoundlyKill) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    declare void @observe(i8**) readonly
    define void @writer(i8** %p) {
      store i8* @B, i8** %p
      ret void
    }
    define i8* @main(i1 %cond) {
      %slot = alloca i8*
      store i8* @A, i8** %slot
      %fp = select i1 %cond, void (i8**)* @writer, void (i8**)* @observe
      call void %fp(i8** %slot)
      %after = load i8*, i8** %slot
      ret i8* %after
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  // Calling @observe preserves @A; calling @writer writes @B. Both must remain!
  expect(analysis, *module, "after", {"A", "B"});
}

TEST(ValueFlowPTATest, LibraryMemsetAndMemcpyRespectByteAndLengthChecks) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    declare i8* @memset(i8*, i32, i64)
    declare i8* @memcpy(i8*, i8*, i64)
    define i8* @main() {
      %s1 = alloca i8*
      store i8* @A, i8** %s1
      %b1 = bitcast i8** %s1 to i8*
      %m1 = call i8* @memset(i8* %b1, i32 255, i64 8)
      %nonZeroSet = load i8*, i8** %s1

      %s2 = alloca i8*
      store i8* @A, i8** %s2
      %b2 = bitcast i8** %s2 to i8*
      %m2 = call i8* @memset(i8* %b2, i32 0, i64 8)
      %zeroSet = load i8*, i8** %s2

      %src = alloca i8*
      store i8* @A, i8** %src
      %srcb = bitcast i8** %src to i8*

      %d1 = alloca i8*
      store i8* @A, i8** %d1
      %d1b = bitcast i8** %d1 to i8*
      %c1 = call i8* @memcpy(i8* %d1b, i8* %srcb, i64 1)
      %partialCopy = load i8*, i8** %d1

      %d2 = alloca i8*
      %d2b = bitcast i8** %d2 to i8*
      %c2 = call i8* @memcpy(i8* %d2b, i8* %srcb, i64 8)
      %fullCopy = load i8*, i8** %d2
      ret i8* %fullCopy
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "nonZeroSet", {"A", "unknown"});
  expect(analysis, *module, "zeroSet", {"A", "null"});
  expect(analysis, *module, "partialCopy", {"A", "unknown"});
  expect(analysis, *module, "fullCopy", {"A"});
}

TEST(ValueFlowPTATest, SharedSwapCallsKeepDisjointLocalsIsolated) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    @C = global i8 0
    @D = global i8 0
    @E = global i8 0
    define void @swap(i8** %p, i8** %q) {
      %s1 = load i8*, i8** %p
      %s2 = load i8*, i8** %q
      store i8* %s2, i8** %p
      store i8* %s1, i8** %q
      ret void
    }
    define i8* @main() {
      %a = alloca i8*
      %b = alloca i8*
      %c = alloca i8*
      %d = alloca i8*
      store i8* @A, i8** %a
      store i8* @B, i8** %b
      store i8* @C, i8** %c
      store i8* @D, i8** %d
      call void @swap(i8** %a, i8** %b)
      %afterFirst = load i8*, i8** %a
      store i8* @E, i8** %a
      call void @swap(i8** %c, i8** %d)
      %afterSecond = load i8*, i8** %a
      ret i8* %afterSecond
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  EXPECT_FALSE(analysis.getPointsTo(value(*module, "afterFirst"))
                   .count(analysis.getObjectId(module->getNamedValue("E"))));
  expect(analysis, *module, "afterSecond", {"E"});
  EXPECT_FALSE(analysis.getStatistics().solver.usedWeakFallback);
}

TEST(ValueFlowPTATest, ExtractElementAndTaggedPtrToIntArithmetic) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    define i8* @main() {
      %vec = insertelement <2 x i8*> undef, i8* @A, i32 0
      %elem = extractelement <2 x i8*> %vec, i32 0

      %slot = alloca i8*
      store i8* @A, i8** %slot
      %asInt = ptrtoint i8* @A to i64
      %tagged = or i64 %asInt, 1
      %slotInt = bitcast i8** %slot to i64*
      store i64 %tagged, i64* %slotInt
      %reloaded = load i8*, i8** %slot
      ret i8* %elem
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "elem", {"A"});
  expect(analysis, *module, "reloaded", {"A", "unknown"});
}

TEST(ValueFlowPTATest, FreshCallocInCalleePreservesNullInitializer) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    declare i8* @calloc(i64, i64)
    define i8* @readFresh() {
      %raw = call i8* @calloc(i64 1, i64 8)
      %slot = bitcast i8* %raw to i8**
      %val = load i8*, i8** %slot
      ret i8* %val
    }
    define i8* @main() {
      %res = call i8* @readFresh()
      ret i8* %res
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "val", {"null"});
  expect(analysis, *module, "res", {"null"});
}

TEST(ValueFlowPTATest, MultiEntryLibraryPropagatesGlobalStateAcrossExports) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    @G = global i8* null
    define i8* @get_g() {
      %loaded = load i8*, i8** @G
      ret i8* %loaded
    }
    define i8* @set_g() {
      store i8* @A, i8** @G
      store i8* @B, i8** @G
      %local = load i8*, i8** @G
      ret i8* %local
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  // Intra-function strong update inside @set_g still kills @A before %local!
  expect(analysis, *module, "local", {"B"});
  // Inter-entry-point driver loop propagates @set_g's final @B write to @get_g
  // (in any call order, including calling @get_g before @set_g -> null).
  expect(analysis, *module, "loaded", {"null", "B"});
}

TEST(ValueFlowPTATest, StrtolGetlineAsprintfByValMismatchAndConstExprPtrToInt) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    @constTagged = global i64 add (i64 ptrtoint (i8* @A to i64), i64 1)
    @byvalRaw = global i8* bitcast (i8* (i8**)* @readByVal to i8*)
    declare i64 @strtol(i8*, i8**, i32)
    declare i64 @getline(i8**, i64*, i8*)
    declare i32 @asprintf(i8**, i8*, ...)
    define i8* @readByVal(i8** byval(i8*) %copy) {
      %bvVal = load i8*, i8** %copy
      ret i8* %bvVal
    }
    define i8* @main(i8* %fp) {
      ; strtol writes arg0 into *arg1 (COPY Arg1 D Arg0 V)
      %endSlot = alloca i8*
      store i8* null, i8** %endSlot
      %n1 = call i64 @strtol(i8* @A, i8** %endSlot, i32 10)
      %endPtr = load i8*, i8** %endSlot

      ; getline writes newly allocated buffer into *arg0
      %lineSlot = alloca i8*
      %cap = alloca i64
      store i8* null, i8** %lineSlot
      %glCall = call i64 @getline(i8** %lineSlot, i64* %cap, i8* %fp)
      %linePtr = load i8*, i8** %lineSlot

      ; asprintf writes newly allocated buffer into *arg0
      %fmtSlot = alloca i8*
      store i8* null, i8** %fmtSlot
      %asCall = call i32 (i8**, i8*, ...) @asprintf(i8** %fmtSlot, i8* @B)
      %fmtPtr = load i8*, i8** %fmtSlot

      ; ConstantExpr add(ptrtoint(@A), 1) in global initializer
      %taggedPtr = bitcast i64* @constTagged to i8**
      %fromTagged = load i8*, i8** %taggedPtr

      ; Mismatched indirect call to a byval callee (0 args) must seed unknown
      %rawFn = load i8*, i8** @byvalRaw
      %badFn = bitcast i8* %rawFn to i8* ()*
      %fromBadByVal = call i8* %badFn()
      ret i8* %endPtr
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "endPtr", {"null", "A"});
  expect(analysis, *module, "linePtr", {"null", "glCall"});
  expect(analysis, *module, "fmtPtr", {"null", "asCall"});
  EXPECT_TRUE(analysis.getPointsTo(value(*module, "fromTagged"))
                  .count(analysis.getUnknownObjectId()));
  EXPECT_TRUE(analysis.getPointsTo(value(*module, "fromBadByVal"))
                  .count(analysis.getUnknownObjectId()));
}

TEST(ValueFlowPTATest, InlineAsmDoesNotTriggerSpuriousAddressTakenCallbacks) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    @g = global i8* @A
    define void @cb() {
      store i8* @B, i8** @g
      ret void
    }
    define i8* @main() {
      %keep = alloca void ()*
      store void ()* @cb, void ()** %keep
      call void asm sideeffect "", ""() readnone
      %afterReadNone = load i8*, i8** @g
      %asmPtr = call i8* asm "", "=r"() readnone
      ret i8* %afterReadNone
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  // Readnone inline asm must NOT invoke unrelated address-taken @cb!
  expect(analysis, *module, "afterReadNone", {"A"});
  expect(analysis, *module, "asmPtr", {"unknown"});
}

TEST(ValueFlowPTATest, AtexitAndSignalInvokeRegisteredHandlersOnly) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    @C = global i8 0
    @D = global i8 0
    @E = global i8 0
    @gExit = global i8* null
    @gSig = global i8* null
    @gPostReg = global i8* null
    @gOther = global i8* null
    @llvm.global_dtors = appending global [1 x { i32, void ()*, i8* }] [
      { i32, void ()*, i8* } { i32 100, void ()* @dtor_check, i8* null }
    ]
    declare i32 @atexit(void ()*)
    declare void (i32)* @signal(i32, void (i32)*)
    define void @onExit() {
      %seenInExit = load i8*, i8** @gPostReg
      store i8* @A, i8** @gExit
      ret void
    }
    define void @onSig(i32 %s) {
      %seenInSig = load i8*, i8** @gPostReg
      store i8* @B, i8** @gSig
      ret void
    }
    define void @dtor_check() {
      %seenInDtor = load i8*, i8** @gExit
      ret void
    }
    define void @unrelated() {
      store i8* @C, i8** @gOther
      ret void
    }
    define void @dead_reg() {
      %d = call i32 @atexit(void ()* @unrelated)
      ret void
    }
    define i8* @main() {
      %keep = alloca void ()*
      store void ()* @unrelated, void ()** %keep
      %r1 = call i32 @atexit(void ()* @onExit)
      %r2 = call void (i32)* @signal(i32 2, void (i32)* @onSig)
      %vExitBeforeRet = load i8*, i8** @gExit
      %vSig = load i8*, i8** @gSig
      %vOther = load i8*, i8** @gOther
      store i8* @D, i8** @gPostReg
      ret i8* %vExitBeforeRet
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  // atexit handler has NOT run yet before main returns, runs after main
  // (observing @D stored after atexit()), and its write @A reaches dtors.
  expect(analysis, *module, "vExitBeforeRet", {"null"});
  expect(analysis, *module, "seenInExit", {"D"});
  expect(analysis, *module, "seenInDtor", {"null", "A"});
  // signal handler may fire immediately (main sees @B) AND after subsequent
  // writes (handler sees both null and @D).
  expect(analysis, *module, "vSig", {"null", "B"});
  expect(analysis, *module, "seenInSig", {"null", "D"});
  // Unrelated / dead-registered handler must NOT run.
  expect(analysis, *module, "vOther", {"null"});
}

TEST(ValueFlowPTATest, ExitCallInvokesRegisteredAtexitHandlersWithCurrentState) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    @C = global i8 0
    @g = global i8* @A
    @h = global i8* null
    declare i32 @atexit(void ()*)
    declare i32 @on_exit(void (i32, i8*)*, i8*)
    declare void @exit(i32) noreturn
    define void @cleanup1() {
      %observed = load i8*, i8** @g
      store i8* @C, i8** @h
      ret void
    }
    define void @cleanup2(i32 %status, i8* %arg) {
      %observedFromH = load i8*, i8** @h
      %observedArg = bitcast i8* %arg to i8*
      ret void
    }
    define i32 @main() {
      %r1 = call i32 @atexit(void ()* @cleanup1)
      %r2 = call i32 @on_exit(void (i32, i8*)* @cleanup2, i8* @A)
      store i8* @B, i8** @g
      call void @exit(i32 1)
      unreachable
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "observed", {"B"});
  expect(analysis, *module, "observedFromH", {"null", "C"});
  expect(analysis, *module, "observedArg", {"A"});
}

TEST(ValueFlowPTATest, OpaqueExternalEscapeFlowsThroughSubsequentDefinedWrapper) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 0
    @B = global i8 0
    declare void @register_slot(i8**)
    declare void @trigger()
    define void @call_trigger() {
      call void @trigger()
      ret void
    }
    define i8* @main() {
      %x = alloca i8*
      %y = alloca i8*
      call void @register_slot(i8** %x)
      store i8* @A, i8** %x
      store i8* @B, i8** %x
      store i8* @A, i8** %y
      call void @call_trigger()
      %xAfter = load i8*, i8** %x
      %yAfter = load i8*, i8** %y
      ret i8* %xAfter
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  // %x escaped to opaque @register_slot, so @call_trigger -> @trigger must
  // clobber %x with unknown (while keeping the pre-call strong kill of @A and
  // NOT injecting %x itself into %xAfter). %y never escaped externally.
  expect(analysis, *module, "xAfter", {"B", "unknown"});
  expect(analysis, *module, "yAfter", {"A"});
  EXPECT_FALSE(analysis.getStatistics().solver.usedWeakFallback);
}

TEST(ValueFlowPTATest, AggregateVectorPtrToIntAndVarArgCarriersPreserveSoundness) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    %struct.Carrier = type { i32, i8** }
    %struct.RetPair = type { i8*, i32 }
    @A = global i8 0
    @B = global i8 0
    @C = global i8 0
    declare void @llvm.va_start(i8*)
    declare void @llvm.va_end(i8*)

    define void @sub_struct(%struct.Carrier %s) {
      %p = extractvalue %struct.Carrier %s, 1
      store i8* @B, i8** %p
      ret void
    }
    define void @use_struct(%struct.Carrier %s) {
      call void @sub_struct(%struct.Carrier %s)
      ret void
    }
    define %struct.RetPair @make_pair() {
      %p0 = insertvalue %struct.RetPair undef, i8* @C, 0
      %p1 = insertvalue %struct.RetPair %p0, i32 42, 1
      ret %struct.RetPair %p1
    }
    define void @use_vec(<2 x i8**> %v) {
      %p = extractelement <2 x i8**> %v, i32 0
      store i8* @B, i8** %p
      ret void
    }
    define void @sub_int(i64 %raw) {
      %p = inttoptr i64 %raw to i8**
      store i8* @B, i8** %p
      ret void
    }
    define void @use_int(i64 %raw) {
      call void @sub_int(i64 %raw)
      ret void
    }
    define void @vconsumer(i8* %ap) {
      %p = va_arg i8* %ap, i8**
      store i8* @B, i8** %p
      ret void
    }
    define void @vhelper(i32 %n, ...) {
      %ap = alloca i8
      call void @llvm.va_start(i8* %ap)
      call void @vconsumer(i8* %ap)
      call void @llvm.va_end(i8* %ap)
      ret void
    }
    define i8* @main() {
      ; 1. Two-hop first-class aggregate argument + aggregate return
      %sSlot = alloca i8*
      store i8* @A, i8** %sSlot
      %c0 = insertvalue %struct.Carrier undef, i32 1, 0
      %c1 = insertvalue %struct.Carrier %c0, i8** %sSlot, 1
      call void @use_struct(%struct.Carrier %c1)
      %afterStruct = load i8*, i8** %sSlot
      %pair = call %struct.RetPair @make_pair()
      %fromRetPair = extractvalue %struct.RetPair %pair, 0

      ; 2. First-class vector-of-pointers argument
      %vSlot = alloca i8*
      store i8* @A, i8** %vSlot
      %vec = insertelement <2 x i8**> undef, i8** %vSlot, i32 0
      call void @use_vec(<2 x i8**> %vec)
      %afterVec = load i8*, i8** %vSlot

      ; 3. Two-hop ptrtoint argument + deep (>6) arithmetic ptrtoint store
      %iSlot = alloca i8*
      store i8* @A, i8** %iSlot
      %i0 = ptrtoint i8** %iSlot to i64
      %i1 = add i64 %i0, 1
      %i2 = add i64 %i1, 1
      %i3 = add i64 %i2, 1
      %i4 = add i64 %i3, 1
      %i5 = add i64 %i4, 1
      %i6 = add i64 %i5, 1
      %i7 = add i64 %i6, 1
      %i8 = add i64 %i7, 1
      call void @use_int(i64 %i8)
      %afterInt = load i8*, i8** %iSlot

      %deepSlot = alloca i8*
      store i8* @A, i8** %deepSlot
      %deepIntPtr = bitcast i8** %deepSlot to i64*
      store i64 %i8, i64* %deepIntPtr
      %afterDeepIntStore = load i8*, i8** %deepSlot

      ; 4. Variadic argument forwarded via va_list to helper
      %vaSlot = alloca i8*
      store i8* @A, i8** %vaSlot
      call void (i32, ...) @vhelper(i32 1, i8** %vaSlot)
      %afterVarArg = load i8*, i8** %vaSlot

      ret i8* %afterStruct
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));
  ValueFlowPTA analysis(*module);
  analysis.analyze();
  expect(analysis, *module, "afterStruct", {"B"});
  expect(analysis, *module, "fromRetPair", {"C"});
  expect(analysis, *module, "afterVec", {"B"});
  expect(analysis, *module, "afterInt", {"A", "B"});
  expect(analysis, *module, "afterDeepIntStore", {"A", "unknown"});
  EXPECT_TRUE(analysis.getPointsTo(value(*module, "afterVarArg"))
                  .count(analysis.getObjectId(module->getNamedValue("B"))));
}

TEST(ValueFlowPTATest, ReadOnlyConstantGlobalImmuneToExternalClobber) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 1
    @B = global i8 2
    @vtable = constant [2 x i8*] [i8* @A, i8* @B]
    @strlit = constant [4 x i8] c"abc\00"
    @mutGlobal = global i8* @A

    declare void @opaque_clobber()

    define i8* @main(i32 %idx) {
      call void @opaque_clobber()
      %gep = getelementptr [2 x i8*], [2 x i8*]* @vtable, i32 0, i32 %idx
      %vtVal = load i8*, i8** %gep
      %mutVal = load i8*, i8** @mutGlobal
      %strPtr = getelementptr [4 x i8], [4 x i8]* @strlit, i32 0, i32 0
      %strAsPtr = bitcast i8* %strPtr to i8**
      %strLoaded = load i8*, i8** %strAsPtr
      ret i8* %vtVal
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));

  ValueFlowPTA analysis(*module);
  analysis.analyze();
  // Constant global with definitive initializer is immune to unknown clobber.
  expect(analysis, *module, "vtVal", {"A", "B"});
  // Empty-initialized constant string literal yields no pointer targets.
  expect(analysis, *module, "strLoaded", {});
  // Mutable global receives unknown from opaque_clobber().
  expect(analysis, *module, "mutVal", {"A", "unknown"});
}

TEST(ValueFlowPTATest, IndirectByValCalleeResolvesInSinglePassWithIsolation) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    %struct.Box = type { i8* }
    @A = global i8 1
    @B = global i8 2
    @fp = global i8* (%struct.Box*)* @callee

    define i8* @callee(%struct.Box* byval(%struct.Box) %arg) {
      %f0 = getelementptr %struct.Box, %struct.Box* %arg, i32 0, i32 0
      %inVal = load i8*, i8** %f0
      store i8* @B, i8** %f0
      %afterMut = load i8*, i8** %f0
      ret i8* %inVal
    }

    define i8* @main() {
      %box = alloca %struct.Box
      %p0 = getelementptr %struct.Box, %struct.Box* %box, i32 0, i32 0
      store i8* @A, i8** %p0
      %fn = load i8* (%struct.Box*)*, i8* (%struct.Box*)** @fp
      %ret = call i8* %fn(%struct.Box* byval(%struct.Box) %box)
      %callerAfter = load i8*, i8** %p0
      ret i8* %ret
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));

  ValueFlowPTA analysis(*module);
  analysis.analyze();
  // Callee-side byval copy resolves dynamically in a single solver pass.
  EXPECT_EQ(analysis.getStatistics().callGraphIterations, 1u);
  expect(analysis, *module, "inVal", {"A"});
  expect(analysis, *module, "ret", {"A"});
  expect(analysis, *module, "afterMut", {"A", "B"});
  // Callee's mutation of its byval copy must not leak back to caller's box.
  expect(analysis, *module, "callerAfter", {"A"});
}

TEST(ValueFlowPTATest, IndirectUnknownTargetResolvesInSinglePass) {
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
    @A = global i8 1
    declare i8* (i8**)* @get_unknown_fn()

    define i8* @main() {
      %slot = alloca i8*
      store i8* @A, i8** %slot
      %fp = call i8* (i8**)* @get_unknown_fn()
      %res = call i8* %fp(i8** %slot)
      %after = load i8*, i8** %slot
      ret i8* %res
    }
  )", diagnostic, context);
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module, &errs()));

  ValueFlowPTA analysis(*module);
  analysis.analyze();
  EXPECT_EQ(analysis.getStatistics().callGraphIterations, 1u);
  expect(analysis, *module, "res", {"unknown"});
  expect(analysis, *module, "after", {"A", "unknown"});
}
