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
  const Function *main = module.getFunction("main");
  for (const BasicBlock &block : *main)
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
