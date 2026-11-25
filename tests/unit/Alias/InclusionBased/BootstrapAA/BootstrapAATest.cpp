#include "Alias/InclusionBased/BootstrapAA/BootstrapAA.h"

#include <stdexcept>

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using lotus::bootstrap::BootstrapAA;
using lotus::bootstrap::Point;
using lotus::bootstrap::PointsToSet;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      throw std::runtime_error(std::string(__FILE__) + ":" +                   \
                               std::to_string(__LINE__) + ": " #x);            \
  } while (false)
namespace {
struct Fixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit Fixture(const char *ir) {
    llvm::SMDiagnostic diagnostic;
    module = llvm::parseAssemblyString(ir, diagnostic, context);
    if (!module) {
      diagnostic.print("BootstrapAALLVMTest", llvm::errs());
      throw std::runtime_error("failed to parse test IR");
    }
  }
  const llvm::Instruction &instruction(const char *function,
                                       const char *name) const {
    const llvm::Function *f = module->getFunction(function);
    CHECK(f);
    for (const llvm::BasicBlock &block : *f)
      for (const llvm::Instruction &i : block)
        if (i.getName() == name)
          return i;
    throw std::runtime_error(std::string("missing instruction ") + name);
  }
};
lotus::bootstrap::Options options() {
  lotus::bootstrap::Options result;
  result.andersen_threshold = 1;
  return result;
}
void callsAndStackStores() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @b = global i8 0
    define i8* @identity(i8* %p) { ret i8* %p }
    define i32 @main() {
      %s = alloca i8*
      store i8* @a, i8** %s
      %old = load i8*, i8** %s
      store i8* @b, i8** %s
      %new = load i8*, i8** %s
      %x = call i8* @identity(i8* @a)
      %y = call i8* @identity(i8* @b)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  aa.precomputeAll();
  auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  auto b = aa.objectId(*f.module->getNamedGlobal("b"));
  const auto &old = f.instruction("main", "old"),
             &now = f.instruction("main", "new");
  CHECK(aa.pointsTo(old, old, {}, Point::After).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(now, now, {}, Point::After).points_to == PointsToSet(b));
  const auto &x = f.instruction("main", "x"), &y = f.instruction("main", "y");
  CHECK(aa.pointsTo(x, x, {}, Point::After).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(y, y, {}, Point::After).points_to == PointsToSet(b));
  const auto *identity = f.module->getFunction("identity");
  const auto &ret = identity->getEntryBlock().back();
  CHECK(aa.pointsTo(*identity->getArg(0), ret, {llvm::cast<llvm::CallBase>(&x)})
            .points_to == PointsToSet(a));
  CHECK(aa.pointsTo(*identity->getArg(0), ret, {llvm::cast<llvm::CallBase>(&y)})
            .points_to == PointsToSet(b));
  CHECK(!aa.mayAlias(old, now, x));
  CHECK(aa.objectInfo(aa.objectId(f.instruction("main", "s"))).singleton);
  CHECK(!aa.hierarchy().successors.empty());
}
void globalsAndExternalHavoc() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @b = global i8 0
    @slot = global i8* @a
    declare void @opaque()
    define i32 @main() {
      %old = load i8*, i8** @slot
      store i8* @b, i8** @slot
      %new = load i8*, i8** @slot
      call void @opaque()
      %unknown = load i8*, i8** @slot
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &old = f.instruction("main", "old"),
             &now = f.instruction("main", "new");
  CHECK(aa.pointsTo(old, old, {}, Point::After).points_to ==
        PointsToSet(aa.objectId(*f.module->getNamedGlobal("a"))));
  CHECK(aa.pointsTo(now, now, {}, Point::After).points_to ==
        PointsToSet(aa.objectId(*f.module->getNamedGlobal("b"))));
  const auto &unknown = f.instruction("main", "unknown");
  CHECK(aa.pointsTo(unknown, unknown, {}, Point::After).points_to.isTop());
}
void partialStoresAndFreeze() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @slot = global i8* @a
    define i32 @main() {
      %byte = bitcast i8** @slot to i8*
      store i8 7, i8* %byte
      %p = load i8*, i8** @slot
      %frozen = freeze i8* undef
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  for (const char *name : {"p", "frozen"}) {
    const auto &i = f.instruction("main", name);
    CHECK(aa.pointsTo(i, i, {}, Point::After).points_to.isTop());
  }
}
void phiAndRecursion() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @b = global i8 0
    define i8* @rec(i1 %c, i8* %p) {
      %s = alloca i8*
      br i1 %c, label %base, label %step
    base:
      ret i8* %p
    step:
      %r = call i8* @rec(i1 %c, i8* %p)
      ret i8* %r
    }
    define i32 @main(i1 %c) {
      br i1 %c, label %left, label %right
    left:
      br label %merge
    right:
      br label %merge
    merge:
      %p = phi i8* [ @a, %left ], [ @b, %right ]
      %r = call i8* @rec(i1 %c, i8* %p)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  PointsToSet expected(aa.objectId(*f.module->getNamedGlobal("a")));
  expected.insert(aa.objectId(*f.module->getNamedGlobal("b")));
  const auto &r = f.instruction("main", "r");
  CHECK(aa.pointsTo(r, r, {}, Point::After).points_to == expected);
  CHECK(!aa.objectInfo(aa.objectId(f.instruction("rec", "s"))).singleton);
}
void byvalCopies() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    %T = type { i8* }
    define void @copy(%T* byval(%T) %arg) {
      %p = bitcast %T* %arg to i8*
      ret void
    }
    define i32 @main() {
      %s = alloca %T
      call void @copy(%T* byval(%T) %s)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &p = f.instruction("copy", "p");
  const auto *arg = f.module->getFunction("copy")->getArg(0);
  auto result = aa.pointsToAllContexts(p, p, Point::After);
  CHECK(result.points_to == PointsToSet(aa.objectId(*arg)));
  CHECK(!result.points_to.contains(aa.objectId(f.instruction("main", "s"))));
}
void allocationsCanFail() {
  Fixture f(R"IR(
    declare i8* @malloc(i64)
    define i32 @main() {
      %p = call i8* @malloc(i64 8)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &p = f.instruction("main", "p");
  auto result = aa.pointsTo(p, p, {}, Point::After);
  CHECK(!result.points_to.isTop());
  CHECK(result.points_to.contains(lotus::bootstrap::NULL_OBJECT));
  CHECK(result.points_to.contains(aa.objectId(p)));
  CHECK(!aa.objectInfo(aa.objectId(p)).singleton);
}
void expandedAllocationModels() {
  Fixture f(R"IR(
    @a = global i8 0
    declare i8* @realloc(i8*, i64)
    declare i8* @aligned_alloc(i64, i64)
    declare i8* @_Znwm(i64)
    define i32 @main() {
      %resized = call i8* @realloc(i8* @a, i64 16)
      %aligned = call i8* @aligned_alloc(i64 16, i64 32)
      %created = call i8* @_Znwm(i64 8)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &resized = f.instruction("main", "resized");
  PointsToSet resizedExpected(aa.objectId(*f.module->getNamedGlobal("a")));
  resizedExpected.insert(aa.objectId(resized));
  resizedExpected.insert(lotus::bootstrap::NULL_OBJECT);
  CHECK(aa.pointsTo(resized, resized, {}, Point::After).points_to ==
        resizedExpected);

  const auto &aligned = f.instruction("main", "aligned");
  PointsToSet alignedExpected(aa.objectId(aligned));
  alignedExpected.insert(lotus::bootstrap::NULL_OBJECT);
  CHECK(aa.pointsTo(aligned, aligned, {}, Point::After).points_to ==
        alignedExpected);

  const auto &created = f.instruction("main", "created");
  CHECK(aa.pointsTo(created, created, {}, Point::After).points_to ==
        PointsToSet(aa.objectId(created)));
}
void zeroLengthMemoryIntrinsic() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @slot = global i8* @a
    declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1 immarg)
    define i32 @main() {
      %bytes = bitcast i8** @slot to i8*
      call void @llvm.memset.p0i8.i64(i8* %bytes, i8 0, i64 0, i1 false)
      %after = load i8*, i8** @slot
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &after = f.instruction("main", "after");
  CHECK(aa.pointsTo(after, after, {}, Point::After).points_to ==
        PointsToSet(aa.objectId(*f.module->getNamedGlobal("a"))));
}
void globalAliasDirectCallee() {
  Fixture f(R"IR(
    @a = global i8 0
    @identity_alias = alias i8* (i8*), i8* (i8*)* @identity
    define i8* @identity(i8* %p) { ret i8* %p }
    define i32 @main() {
      %result = call i8* @identity_alias(i8* @a)
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &result = f.instruction("main", "result");
  CHECK(aa.pointsTo(result, result, {}, Point::After).points_to ==
        PointsToSet(aa.objectId(*f.module->getNamedGlobal("a"))));
}
void readOnlyInteriorPointerModel() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @slot = global i8* @a
    declare i8* @strchr(i8*, i32)
    define i32 @main() {
      %found = call i8* @strchr(i8* @a, i32 0)
      %after = load i8*, i8** @slot
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto &found = f.instruction("main", "found");
  PointsToSet expected(a);
  expected.insert(lotus::bootstrap::NULL_OBJECT);
  CHECK(aa.pointsTo(found, found, {}, Point::After).points_to == expected);
  const auto &after = f.instruction("main", "after");
  CHECK(aa.pointsTo(after, after, {}, Point::After).points_to ==
        PointsToSet(a));
}
void vectorPointerOperationsAreConservative() {
  Fixture f(R"IR(
    @a = global i8 0
    define i32 @main() {
      %addresses = getelementptr i8, i8* @a, <2 x i64> <i64 0, i64 1>
      %element = extractelement <2 x i8*> %addresses, i32 0
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &element = f.instruction("main", "element");
  CHECK(aa.pointsTo(element, element, {}, Point::After).points_to.isTop());
}

void mainArgvPreservesPartitioning() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @b = global i8 0
    define i32 @main(i32 %argc, i8** %argv) {
      %s1 = alloca i8*
      %s2 = alloca i8*
      store i8* @a, i8** %s1
      store i8* @b, i8** %s2
      %p1 = load i8*, i8** %s1
      %p2 = load i8*, i8** %s2
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  // argv (top) must NOT collapse all unrelated partitions into 1 giant cluster!
  CHECK(aa.statistics().steensgaard_partitions >= 4);
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto b = aa.objectId(*f.module->getNamedGlobal("b"));
  const auto &p1 = f.instruction("main", "p1");
  const auto &p2 = f.instruction("main", "p2");
  const auto &ret = f.module->getFunction("main")->getEntryBlock().back();
  CHECK(aa.pointsTo(p1, p1, {}, Point::After).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(p2, p2, {}, Point::After).points_to == PointsToSet(b));
  CHECK(!aa.mayAlias(p1, p2, ret));
}

void fieldSensitiveStructsAndStrongUpdates() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.S = type { i32, i8*, i8* }
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0
    @gs = global %struct.S { i32 7, i8* @a, i8* @b }
    define i32 @main() {
      %g1 = load i8*, i8** getelementptr inbounds (%struct.S, %struct.S* @gs, i32 0, i32 1)
      %g2 = load i8*, i8** getelementptr inbounds (%struct.S, %struct.S* @gs, i32 0, i32 2)
      %s = alloca %struct.S
      %tag = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      %f1 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 1
      %f2 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 2
      store i8* @a, i8** %f1
      store i8* @b, i8** %f2
      store i32 42, i32* %tag
      %v1 = load i8*, i8** %f1
      %v2 = load i8*, i8** %f2
      store i8* @c, i8** %f1
      %v1_after = load i8*, i8** %f1
      %v2_after = load i8*, i8** %f2
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto b = aa.objectId(*f.module->getNamedGlobal("b"));
  const auto c = aa.objectId(*f.module->getNamedGlobal("c"));
  const auto &g1 = f.instruction("main", "g1");
  const auto &g2 = f.instruction("main", "g2");
  const auto &v1 = f.instruction("main", "v1");
  const auto &v2 = f.instruction("main", "v2");
  const auto &v1_after = f.instruction("main", "v1_after");
  const auto &v2_after = f.instruction("main", "v2_after");
  const auto &ret = f.module->getFunction("main")->getEntryBlock().back();
  CHECK(aa.pointsTo(g1, g1, {}, Point::After).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(g2, g2, {}, Point::After).points_to == PointsToSet(b));
  CHECK(aa.pointsTo(v1, v1, {}, Point::After).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(v2, v2, {}, Point::After).points_to == PointsToSet(b));
  CHECK(aa.pointsTo(v1_after, v1_after, {}, Point::After).points_to ==
        PointsToSet(c));
  CHECK(aa.pointsTo(v2_after, v2_after, {}, Point::After).points_to ==
        PointsToSet(b));
  CHECK(!aa.mayAlias(v1_after, v2_after, ret));
  CHECK(aa.fieldObjectId(f.instruction("main", "s"), 8) !=
        lotus::bootstrap::INVALID);
  CHECK(aa.fieldObjectId(f.instruction("main", "s"), 16) !=
        lotus::bootstrap::INVALID);
}

void nestedStructsAndMemCopy() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.Inner = type { i8*, i8* }
    %struct.Outer = type { %struct.Inner, i8* }
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1 immarg)
    define i32 @main() {
      %src = alloca %struct.Outer
      %dst = alloca %struct.Outer
      %s0 = getelementptr inbounds %struct.Outer, %struct.Outer* %src, i32 0, i32 0, i32 0
      %s1 = getelementptr inbounds %struct.Outer, %struct.Outer* %src, i32 0, i32 0, i32 1
      %s2 = getelementptr inbounds %struct.Outer, %struct.Outer* %src, i32 0, i32 1
      store i8* @a, i8** %s0
      store i8* @b, i8** %s1
      store i8* @c, i8** %s2
      %ds = bitcast %struct.Outer* %dst to i8*
      %ss = bitcast %struct.Outer* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %ds, i8* %ss, i64 24, i1 false)
      %d0 = getelementptr inbounds %struct.Outer, %struct.Outer* %dst, i32 0, i32 0, i32 0
      %d1 = getelementptr inbounds %struct.Outer, %struct.Outer* %dst, i32 0, i32 0, i32 1
      %d2 = getelementptr inbounds %struct.Outer, %struct.Outer* %dst, i32 0, i32 1
      %r0 = load i8*, i8** %d0
      %r1 = load i8*, i8** %d1
      %r2 = load i8*, i8** %d2
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto b = aa.objectId(*f.module->getNamedGlobal("b"));
  const auto c = aa.objectId(*f.module->getNamedGlobal("c"));
  const auto &r0 = f.instruction("main", "r0");
  const auto &r1 = f.instruction("main", "r1");
  const auto &r2 = f.instruction("main", "r2");
  CHECK(aa.pointsTo(r0, r0, {}, Point::After).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(r1, r1, {}, Point::After).points_to == PointsToSet(b));
  CHECK(aa.pointsTo(r2, r2, {}, Point::After).points_to == PointsToSet(c));
}

void heapStructAndLinkedList() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.Node = type { i8*, %struct.Node* }
    @a = global i8 0
    declare i8* @malloc(i64)
    define i32 @main() {
      %raw = call i8* @malloc(i64 16)
      %n = bitcast i8* %raw to %struct.Node*
      %dp = getelementptr inbounds %struct.Node, %struct.Node* %n, i32 0, i32 0
      %np = getelementptr inbounds %struct.Node, %struct.Node* %n, i32 0, i32 1
      store i8* @a, i8** %dp
      store %struct.Node* %n, %struct.Node** %np
      %data = load i8*, i8** %dp
      %next = load %struct.Node*, %struct.Node** %np
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto heap = aa.objectId(f.instruction("main", "raw"));
  const auto &data = f.instruction("main", "data");
  const auto &next = f.instruction("main", "next");
  const auto &ret = f.module->getFunction("main")->getEntryBlock().back();
  CHECK(aa.pointsTo(data, data, {}, Point::After).points_to == PointsToSet(a));
  PointsToSet expectedNext(heap);
  expectedNext.insert(lotus::bootstrap::NULL_OBJECT);
  CHECK(aa.pointsTo(next, next, {}, Point::After).points_to == expectedNext);
  CHECK(!aa.mayAlias(data, next, ret));
}

void heapNewStructExactFields() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.Node = type { i8*, %struct.Node* }
    @a = global i8 0
    declare i8* @_Znwm(i64)
    define i32 @main() {
      %raw = call i8* @_Znwm(i64 16)
      %n = bitcast i8* %raw to %struct.Node*
      %dp = getelementptr inbounds %struct.Node, %struct.Node* %n, i32 0, i32 0
      %np = getelementptr inbounds %struct.Node, %struct.Node* %n, i32 0, i32 1
      store i8* @a, i8** %dp
      store %struct.Node* %n, %struct.Node** %np
      %data = load i8*, i8** %dp
      %next = load %struct.Node*, %struct.Node** %np
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto heap = aa.objectId(f.instruction("main", "raw"));
  const auto &data = f.instruction("main", "data");
  const auto &next = f.instruction("main", "next");
  const auto &ret = f.module->getFunction("main")->getEntryBlock().back();
  CHECK(aa.pointsTo(data, data, {}, Point::After).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(next, next, {}, Point::After).points_to ==
        PointsToSet(heap));
  CHECK(!aa.mayAlias(data, next, ret));
}

void arrayOfStructsVariableIndex() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.Pair = type { i8*, i8* }
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0
    define i32 @main(i64 %idx) {
      %arr = alloca [4 x %struct.Pair]
      %e0_f0 = getelementptr inbounds [4 x %struct.Pair], [4 x %struct.Pair]* %arr, i64 0, i64 0, i32 0
      %e1_f0 = getelementptr inbounds [4 x %struct.Pair], [4 x %struct.Pair]* %arr, i64 0, i64 1, i32 0
      %e2_f1 = getelementptr inbounds [4 x %struct.Pair], [4 x %struct.Pair]* %arr, i64 0, i64 2, i32 1
      store i8* @a, i8** %e0_f0
      store i8* @b, i8** %e1_f0
      store i8* @c, i8** %e2_f1
      %ei = getelementptr inbounds [4 x %struct.Pair], [4 x %struct.Pair]* %arr, i64 0, i64 %idx
      %ei_f0 = getelementptr inbounds %struct.Pair, %struct.Pair* %ei, i32 0, i32 0
      %ei_f1 = getelementptr inbounds %struct.Pair, %struct.Pair* %ei, i32 0, i32 1
      %v0 = load i8*, i8** %ei_f0
      %v1 = load i8*, i8** %ei_f1
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto b = aa.objectId(*f.module->getNamedGlobal("b"));
  const auto c = aa.objectId(*f.module->getNamedGlobal("c"));
  PointsToSet ab(a);
  ab.insert(b);
  const auto &v0 = f.instruction("main", "v0");
  const auto &v1 = f.instruction("main", "v1");
  const auto &ret = f.module->getFunction("main")->getEntryBlock().back();
  CHECK(aa.pointsTo(v0, v0, {}, Point::After).points_to == ab);
  CHECK(aa.pointsTo(v1, v1, {}, Point::After).points_to == PointsToSet(c));
  CHECK(!aa.mayAlias(v0, v1, ret));
}

void partialByteOverwriteInsideStructIsSound() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.Pair = type { i8*, i8* }
    @a = global i8 0
    @b = global i8 0
    define i32 @main() {
      %s = alloca %struct.Pair
      %f0 = getelementptr inbounds %struct.Pair, %struct.Pair* %s, i32 0, i32 0
      %f1 = getelementptr inbounds %struct.Pair, %struct.Pair* %s, i32 0, i32 1
      store i8* @a, i8** %f0
      store i8* @b, i8** %f1
      %raw = bitcast %struct.Pair* %s to i8*
      store i8 1, i8* %raw
      %r0 = load i8*, i8** %f0
      %r1 = load i8*, i8** %f1
      %raw9 = getelementptr inbounds i8, i8* %raw, i64 9
      store i8 2, i8* %raw9
      %r1_after = load i8*, i8** %f1
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto b = aa.objectId(*f.module->getNamedGlobal("b"));
  const auto &r0 = f.instruction("main", "r0");
  const auto &r1 = f.instruction("main", "r1");
  const auto &r1_after = f.instruction("main", "r1_after");
  // Byte write at offset 0 havocs f0 ([0,8)) to top, leaves f1 ([8,16)) exact!
  CHECK(aa.pointsTo(r0, r0, {}, Point::After).points_to.isTop());
  CHECK(aa.pointsTo(r1, r1, {}, Point::After).points_to == PointsToSet(b));
  // Byte write at offset 9 overlaps f1 ([8,16)) -> conservatively top!
  CHECK(aa.pointsTo(r1_after, r1_after, {}, Point::After).points_to.isTop());
}

void indirectFunctionPointersInStructWithoutCollapse() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.Ops = type { i8* ()*, i8* ()* }
    @a = global i8 0
    @b = global i8 0
    define i8* @ret_a() { ret i8* @a }
    define i8* @ret_b() { ret i8* @b }
    define i32 @main() {
      %ops = alloca %struct.Ops
      %p0 = getelementptr inbounds %struct.Ops, %struct.Ops* %ops, i32 0, i32 0
      %p1 = getelementptr inbounds %struct.Ops, %struct.Ops* %ops, i32 0, i32 1
      store i8* ()* @ret_a, i8* ()** %p0
      store i8* ()* @ret_b, i8* ()** %p1
      %f0 = load i8* ()*, i8* ()** %p0
      %f1 = load i8* ()*, i8* ()** %p1
      %ra = call i8* %f0()
      %rb = call i8* %f1()
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto b = aa.objectId(*f.module->getNamedGlobal("b"));
  const auto &ra = f.instruction("main", "ra");
  const auto &rb = f.instruction("main", "rb");
  const auto &ret = f.module->getFunction("main")->getEntryBlock().back();
  CHECK(aa.statistics().steensgaard_partitions >= 4);
  CHECK(aa.pointsTo(ra, ra, {}, Point::After).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(rb, rb, {}, Point::After).points_to == PointsToSet(b));
  CHECK(!aa.mayAlias(ra, rb, ret));
}

void llvmPathSensitiveCorrelatedBranches() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    @a = global i8 0
    @b = global i8 0
    define i32 @main(i32 %flag) {
    entry:
      %slot = alloca i8*
      %c1 = icmp eq i32 %flag, 0
      br i1 %c1, label %t1, label %f1
    t1:
      store i8* @a, i8** %slot
      br label %join
    f1:
      store i8* @b, i8** %slot
      br label %join
    join:
      %c2 = icmp ne i32 %flag, 0
      br i1 %c2, label %f2, label %t2
    t2:
      %only_a = load i8*, i8** %slot
      br label %done
    f2:
      %only_b = load i8*, i8** %slot
      br label %done
    done:
      ret i32 0
    }
  )IR");
  auto a_opt = options();
  a_opt.path_sensitive = false;
  BootstrapAA insensitive(*f.module, nullptr, a_opt);
  const auto a = insensitive.objectId(*f.module->getNamedGlobal("a"));
  const auto b = insensitive.objectId(*f.module->getNamedGlobal("b"));
  PointsToSet ab(a);
  ab.insert(b);
  const auto &only_a = f.instruction("main", "only_a");
  const auto &only_b = f.instruction("main", "only_b");
  CHECK(insensitive.pointsTo(only_a, only_a, {}, Point::After).points_to == ab);

  auto ps_opt = options();
  ps_opt.path_sensitive = true;
  BootstrapAA sensitive(*f.module, nullptr, ps_opt);
  CHECK(sensitive.pointsTo(only_a, only_a, {}, Point::After).points_to ==
        PointsToSet(a));
  CHECK(sensitive.pointsTo(only_b, only_b, {}, Point::After).points_to ==
        PointsToSet(b));
  CHECK(sensitive.statistics().path_infeasible_prunes > 0);
}

void llvmAliasesAPI() {
  Fixture f(R"IR(
    target datalayout = "e-p:64:64"
    @a = global i8 0
    @b = global i8 0
    define i32 @main() {
      %s = alloca i8*
      store i8* @a, i8** %s
      %p = load i8*, i8** %s
      %q = bitcast i8* %p to i8*
      %r = bitcast i8* @b to i8*
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto &p = f.instruction("main", "p");
  const auto &q = f.instruction("main", "q");
  const auto &r = f.instruction("main", "r");
  const auto &ret = f.module->getFunction("main")->getEntryBlock().back();
  auto res = aa.aliases(p, ret);
  CHECK(res.reachable());
  CHECK(std::find(res.aliases.begin(), res.aliases.end(), &p) !=
        res.aliases.end());
  CHECK(std::find(res.aliases.begin(), res.aliases.end(), &q) !=
        res.aliases.end());
  CHECK(std::find(res.aliases.begin(), res.aliases.end(), &r) ==
        res.aliases.end());
}

void fieldObjectIdAndCrossBackendConsistency() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.Box = type { i8*, i8* }
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0
    define void @swap_fields(%struct.Box* %box) {
      %f0 = getelementptr inbounds %struct.Box, %struct.Box* %box, i32 0, i32 0
      %f1 = getelementptr inbounds %struct.Box, %struct.Box* %box, i32 0, i32 1
      %v0 = load i8*, i8** %f0
      %v1 = load i8*, i8** %f1
      store i8* %v1, i8** %f0
      store i8* %v0, i8** %f1
      ret void
    }
    define i32 @main() {
      %box = alloca %struct.Box
      %f0 = getelementptr inbounds %struct.Box, %struct.Box* %box, i32 0, i32 0
      %f1 = getelementptr inbounds %struct.Box, %struct.Box* %box, i32 0, i32 1
      store i8* @a, i8** %f0
      store i8* @b, i8** %f1
      call void @swap_fields(%struct.Box* %box)
      %after0 = load i8*, i8** %f0
      %after1 = load i8*, i8** %f1
      ret i32 0
    }
  )IR");
  auto g_opt = options();
  g_opt.solver_mode = lotus::bootstrap::SolverMode::GuardedSummary;
  g_opt.enable_one_level_flow = true;
  BootstrapAA guarded(*f.module, nullptr, g_opt);

  auto t_opt = options();
  t_opt.solver_mode = lotus::bootstrap::SolverMode::InputStateTabulation;
  BootstrapAA tab(*f.module, nullptr, t_opt);

  const auto a = guarded.objectId(*f.module->getNamedGlobal("a"));
  const auto b = guarded.objectId(*f.module->getNamedGlobal("b"));
  const auto &box = f.instruction("main", "box");
  const auto &f0 = f.instruction("main", "f0");
  const auto &f1 = f.instruction("main", "f1");
  const auto &after0 = f.instruction("main", "after0");
  const auto &after1 = f.instruction("main", "after1");
  const auto &ret = f.module->getFunction("main")->getEntryBlock().back();

  // Verify fieldObjectId exposes distinct flattened field IDs at offsets 0 & 8!
  const lotus::bootstrap::Id obj0 = guarded.fieldObjectId(box, 0);
  const lotus::bootstrap::Id obj8 = guarded.fieldObjectId(box, 8);
  CHECK(obj0 != obj8);
  CHECK(guarded.pointsTo(f0, ret).points_to == PointsToSet(obj0));
  CHECK(guarded.pointsTo(f1, ret).points_to == PointsToSet(obj8));

  // After swap_fields(&box), field 0 holds @b and field 1 holds @a!
  CHECK(guarded.pointsTo(after0, after0, {}, Point::After).points_to ==
        PointsToSet(b));
  CHECK(guarded.pointsTo(after1, after1, {}, Point::After).points_to ==
        PointsToSet(a));
  CHECK(guarded.pointsTo(after0, ret).points_to ==
        tab.pointsTo(after0, ret).points_to);
  CHECK(guarded.pointsTo(after1, ret).points_to ==
        tab.pointsTo(after1, ret).points_to);
  CHECK(!guarded.mayAlias(after0, after1, ret));
}

void llvmInterproceduralPathSensitiveArgAndGlobal() {
  // Part A: Shared argument across caller -> callee correlates branches in
  // both downward (summary) and upward (caller-spliced) ROBDD path sensitivity.
  {
    Fixture f(R"IR(
      target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
      @good = global i8 0
      @bad  = global i8 0
      define void @helper(i32 %f, i8** %slot) {
      entry:
        %c = icmp ne i32 %f, 0
        br i1 %c, label %infeas, label %feas
      infeas:
        store i8* @bad, i8** %slot
        %load_infeas = load i8*, i8** %slot
        ret void
      feas:
        store i8* @good, i8** %slot
        %load_feas = load i8*, i8** %slot
        ret void
      }
      define i32 @main(i32 %flag) {
      entry:
        %slot = alloca i8*
        store i8* @good, i8** %slot
        %c = icmp eq i32 %flag, 0
        br i1 %c, label %docall, label %skip
      docall:
        %cs = bitcast i8** %slot to i8**
        call void @helper(i32 %flag, i8** %cs)
        %after = load i8*, i8** %slot
        ret i32 0
      skip:
        ret i32 0
      }
    )IR");

    auto opt_ps = options();
    opt_ps.path_sensitive = true;
    opt_ps.path_sensitive_interprocedural = true;
    BootstrapAA aa_ps(*f.module, nullptr, opt_ps);

    const auto good = aa_ps.objectId(*f.module->getNamedGlobal("good"));
    const auto bad = aa_ps.objectId(*f.module->getNamedGlobal("bad"));
    const auto &after = f.instruction("main", "after");
    const auto &load_infeas = f.instruction("helper", "load_infeas");
    const auto &load_feas = f.instruction("helper", "load_feas");
    const llvm::CallBase *call_site = nullptr;
    for (const auto &bb : *f.module->getFunction("main"))
      for (const auto &inst : bb)
        if (const auto *cb = llvm::dyn_cast<llvm::CallBase>(&inst))
          call_site = cb;
    CHECK(call_site != nullptr);

    // Downward summary correlation prunes @bad!
    CHECK(aa_ps.pointsTo(after, after, {}, Point::After).points_to ==
          PointsToSet(good));
    // Upward caller splicing prunes the infeasible branch inside @helper!
    CHECK(aa_ps.pointsTo(load_infeas, load_infeas, {call_site}, Point::After)
              .status == lotus::bootstrap::QueryStatus::Unreachable);
    CHECK(aa_ps.pointsToAllContexts(load_infeas, load_infeas, Point::After)
              .status == lotus::bootstrap::QueryStatus::Unreachable);
    CHECK(aa_ps.pointsTo(load_feas, load_feas, {call_site}, Point::After)
              .points_to == PointsToSet(good));
    CHECK(aa_ps.pointsToAllContexts(load_feas, load_feas, Point::After)
              .points_to == PointsToSet(good));

    // Without path sensitivity, both @good and @bad appear.
    auto opt_nops = options();
    opt_nops.path_sensitive = false;
    BootstrapAA aa_nops(*f.module, nullptr, opt_nops);
    PointsToSet both;
    both.insert(good);
    both.insert(bad);
    CHECK(aa_nops.pointsTo(after, after, {}, Point::After).points_to == both);
  }

  // Part B: Non-escaping internal global flag correlates across functions when
  // read-only, and is soundly killed when mutated by a callee.
  {
    Fixture f(R"IR(
      target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
      @g_flag = internal global i32 0
      @good = global i8 0
      @bad  = global i8 0
      define void @reader(i8** %slot) {
      entry:
        %v = load i32, i32* @g_flag
        %c = icmp ne i32 %v, 0
        br i1 %c, label %bad_br, label %good_br
      bad_br:
        store i8* @bad, i8** %slot
        %bad_ld = load i8*, i8** %slot
        ret void
      good_br:
        store i8* @good, i8** %slot
        ret void
      }
      define void @mutator(i8** %slot) {
      entry:
        store i32 1, i32* @g_flag
        store i8* @good, i8** %slot
        ret void
      }
      define i32 @main() {
      entry:
        %slot = alloca i8*
        store i8* @bad, i8** %slot
        %v0 = load i32, i32* @g_flag
        %c0 = icmp eq i32 %v0, 0
        br i1 %c0, label %step1, label %done
      step1:
        call void @reader(i8** %slot)
        %after_read = load i8*, i8** %slot
        call void @mutator(i8** %slot)
        %v1 = load i32, i32* @g_flag
        %c1 = icmp ne i32 %v1, 0
        br i1 %c1, label %after_mut_br, label %done
      after_mut_br:
        %after_mut = load i8*, i8** %slot
        ret i32 0
      done:
        ret i32 0
      }
    )IR");

    auto opt = options();
    opt.path_sensitive = true;
    opt.path_sensitive_interprocedural = true;
    BootstrapAA aa(*f.module, nullptr, opt);

    const auto good = aa.objectId(*f.module->getNamedGlobal("good"));
    const auto &after_read = f.instruction("main", "after_read");
    const auto &after_mut = f.instruction("main", "after_mut");
    const auto &bad_ld = f.instruction("reader", "bad_ld");

    // @reader does not mutate @g_flag -> @bad is pruned both downward & upward.
    CHECK(aa.pointsTo(after_read, after_read, {}, Point::After).points_to ==
          PointsToSet(good));
    CHECK(aa.pointsToAllContexts(bad_ld, bad_ld, Point::After).status ==
          lotus::bootstrap::QueryStatus::Unreachable);
    // @mutator writes to @g_flag -> post-call branch `@g_flag != 0` MUST stay
    // reachable (soundness!).
    auto mut_res = aa.pointsTo(after_mut, after_mut, {}, Point::After);
    CHECK(mut_res.reachable());
    CHECK(mut_res.points_to == PointsToSet(good));
  }
}

void llvmInterproceduralPathSensitiveArgScopingSoundness() {
  // Verify formal-argument predicates are scoped to a single function
  // activation (killed at function entry):
  //  1. Two distinct calls `writer(0, p, @a)` then `writer(1, p, @b)` must NOT
  //     conflate `%k` across invocations (must retain `@a`).
  //  2. Recursive `rec(0, p)` calling `rec(1, p)` must NOT conflate `%flag`
  //     across recursive frames (must reach `store @c, p`).
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    @init = global i8 0
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0
    define void @writer(i32 %k, i8** %p, i8* %val) {
    entry:
      %cond = icmp eq i32 %k, 0
      br i1 %cond, label %write, label %done
    write:
      store i8* %val, i8** %p
      br label %done
    done:
      ret void
    }
    define void @rec(i32 %flag, i8** %p) {
    entry:
      %cond = icmp eq i32 %flag, 0
      br i1 %cond, label %recurse, label %leaf
    recurse:
      call void @rec(i32 1, i8** %p)
      ret void
    leaf:
      store i8* @c, i8** %p
      ret void
    }
    define i32 @main() {
    entry:
      %p = alloca i8*
      store i8* @init, i8** %p
      call void @writer(i32 0, i8** %p, i8* @a)
      call void @writer(i32 1, i8** %p, i8* @b)
      %after_writers = load i8*, i8** %p
      %q = alloca i8*
      store i8* @init, i8** %q
      call void @rec(i32 0, i8** %q)
      %after_rec = load i8*, i8** %q
      ret i32 0
    }
  )IR");

  auto opt = options();
  opt.path_sensitive = true;
  opt.path_sensitive_interprocedural = true;
  BootstrapAA aa(*f.module, nullptr, opt);

  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto c = aa.objectId(*f.module->getNamedGlobal("c"));
  const auto &after_writers = f.instruction("main", "after_writers");
  const auto &after_rec = f.instruction("main", "after_rec");

  auto w_res = aa.pointsTo(after_writers, after_writers, {}, Point::After);
  CHECK(w_res.reachable());
  CHECK(w_res.points_to.contains(a));

  auto r_res = aa.pointsTo(after_rec, after_rec, {}, Point::After);
  CHECK(r_res.reachable());
  CHECK(r_res.points_to.contains(c));
}
} // namespace
TEST(BootstrapAATest, CallsAndStackStores) { callsAndStackStores(); }
TEST(BootstrapAATest, GlobalsAndExternalHavoc) { globalsAndExternalHavoc(); }
TEST(BootstrapAATest, PartialStoresAndFreeze) { partialStoresAndFreeze(); }
TEST(BootstrapAATest, PhiAndRecursion) { phiAndRecursion(); }
TEST(BootstrapAATest, ByvalCopies) { byvalCopies(); }
TEST(BootstrapAATest, AllocationsCanFail) { allocationsCanFail(); }
TEST(BootstrapAATest, ExpandedAllocationModels) { expandedAllocationModels(); }
TEST(BootstrapAATest, ZeroLengthMemoryIntrinsic) {
  zeroLengthMemoryIntrinsic();
}
TEST(BootstrapAATest, GlobalAliasDirectCallee) { globalAliasDirectCallee(); }
TEST(BootstrapAATest, ReadOnlyInteriorPointerModel) {
  readOnlyInteriorPointerModel();
}
TEST(BootstrapAATest, VectorPointerOperationsAreConservative) {
  vectorPointerOperationsAreConservative();
}
TEST(BootstrapAATest, MainArgvPreservesPartitioning) {
  mainArgvPreservesPartitioning();
}
TEST(BootstrapAATest, FieldSensitiveStructsAndStrongUpdates) {
  fieldSensitiveStructsAndStrongUpdates();
}
TEST(BootstrapAATest, NestedStructsAndMemCopy) { nestedStructsAndMemCopy(); }
TEST(BootstrapAATest, HeapStructAndLinkedList) { heapStructAndLinkedList(); }
TEST(BootstrapAATest, HeapNewStructExactFields) { heapNewStructExactFields(); }
TEST(BootstrapAATest, ArrayOfStructsVariableIndex) {
  arrayOfStructsVariableIndex();
}
TEST(BootstrapAATest, PartialByteOverwriteInsideStructIsSound) {
  partialByteOverwriteInsideStructIsSound();
}
TEST(BootstrapAATest, IndirectFunctionPointersInStructWithoutCollapse) {
  indirectFunctionPointersInStructWithoutCollapse();
}
TEST(BootstrapAATest, LLVMPathSensitiveCorrelatedBranches) {
  llvmPathSensitiveCorrelatedBranches();
}
TEST(BootstrapAATest, LLVMAliasesAPI) { llvmAliasesAPI(); }
TEST(BootstrapAATest, FieldObjectIdAndCrossBackendConsistency) {
  fieldObjectIdAndCrossBackendConsistency();
}
TEST(BootstrapAATest, LLVMInterproceduralPathSensitiveArgAndGlobal) {
  llvmInterproceduralPathSensitiveArgAndGlobal();
}
TEST(BootstrapAATest, LLVMInterproceduralPathSensitiveArgScopingSoundness) {
  llvmInterproceduralPathSensitiveArgScopingSoundness();
}
