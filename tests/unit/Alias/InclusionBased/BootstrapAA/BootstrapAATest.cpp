#include "Alias/InclusionBased/BootstrapAA/BootstrapAA.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

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

void llvmCalleeInLoopGuardSoundness() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0
    define void @maybe_write(i8** %p, i8* %v) {
      store i8* %v, i8** %p
      ret void
    }
    define i32 @main(i32 %n) {
    entry:
      %x = alloca i8*
      %y = alloca i8*
      store i8* @a, i8** %x
      store i8* @b, i8** %y
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %ptr = phi i8** [ %x, %entry ], [ %y, %loop ]
      call void @maybe_write(i8** %ptr, i8* @c)
      %next = add i32 %i, 1
      %cond = icmp slt i32 %next, %n
      br i1 %cond, label %loop, label %exit
    exit:
      %res = load i8*, i8** %x
      ret i32 0
    }
  )IR");
  auto gOpt = options();
  gOpt.solver_mode = lotus::bootstrap::SolverMode::GuardedSummary;
  auto tOpt = options();
  tOpt.solver_mode = lotus::bootstrap::SolverMode::InputStateTabulation;

  BootstrapAA guarded(*f.module, nullptr, gOpt);
  BootstrapAA tab(*f.module, nullptr, tOpt);

  const auto c = guarded.objectId(*f.module->getNamedGlobal("c"));
  const auto &res = f.instruction("main", "res");
  auto gr = guarded.pointsTo(res, res, {}, Point::After);
  auto tr = tab.pointsTo(res, res, {}, Point::After);
  CHECK(gr.reachable());
  CHECK(gr.points_to.contains(c));
  CHECK(gr.points_to == tr.points_to);
}

void llvmInterproceduralUpwardGlobalFlagMutationSoundness() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    @g_flag = internal global i32 0
    @init = global i8 0
    @target = global i8 0
    define void @helper(i8** %slot) {
    entry:
      %v = load i32, i32* @g_flag
      %c = icmp ne i32 %v, 0
      br i1 %c, label %active, label %done
    active:
      store i8* @target, i8** %slot
      %ld = load i8*, i8** %slot
      ret void
    done:
      ret void
    }
    define i32 @main() {
    entry:
      %slot = alloca i8*
      store i8* @init, i8** %slot
      %v0 = load i32, i32* @g_flag
      %c0 = icmp eq i32 %v0, 0
      br i1 %c0, label %mutate_and_call, label %exit
    mutate_and_call:
      store i32 1, i32* @g_flag
      call void @helper(i8** %slot)
      %after = load i8*, i8** %slot
      ret i32 0
    exit:
      ret i32 0
    }
  )IR");
  auto opt = options();
  opt.path_sensitive = true;
  opt.path_sensitive_interprocedural = true;
  BootstrapAA aa(*f.module, nullptr, opt);

  const auto target = aa.objectId(*f.module->getNamedGlobal("target"));
  const auto &ld = f.instruction("helper", "ld");
  const auto &after = f.instruction("main", "after");
  const llvm::CallBase *cs = nullptr;
  for (const auto &bb : *f.module->getFunction("main"))
    for (const auto &inst : bb)
      if (const auto *cb = llvm::dyn_cast<llvm::CallBase>(&inst))
        cs = cb;
  CHECK(cs != nullptr);

  // @main mutated @g_flag = 1 before calling @helper, so @helper's
  // `@g_flag != 0` branch MUST remain reachable (both FSCS and FSCI)!
  auto rFscs = aa.pointsTo(ld, ld, {cs}, Point::After);
  CHECK(rFscs.reachable());
  CHECK(rFscs.points_to == PointsToSet(target));

  auto rFsci = aa.pointsToAllContexts(ld, ld, Point::After);
  CHECK(rFsci.reachable());
  CHECK(rFsci.points_to == PointsToSet(target));

  auto rAfter = aa.pointsTo(after, after, {}, Point::After);
  CHECK(rAfter.reachable());
  CHECK(rAfter.points_to.contains(target));
}

void llvmNonZeroOffsetMemCopyAndUnknownLenMemsetSliceSoundness() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.S = type { i64, i64, i8* }
    @a = global i8 0
    @b = global i8 0
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1 immarg)
    declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1 immarg)
    define i32 @main(i64 %n) {
      %src = alloca %struct.S
      %dst = alloca %struct.S
      %dst_p = getelementptr inbounds %struct.S, %struct.S* %dst, i32 0, i32 2
      %src_p = getelementptr inbounds %struct.S, %struct.S* %src, i32 0, i32 2
      store i8* @a, i8** %dst_p
      store i8* @b, i8** %src_p
      ; Copy 16 bytes starting at offset 8 (covering field 1 and field 2!)
      %dst_mid = getelementptr inbounds %struct.S, %struct.S* %dst, i32 0, i32 1
      %src_mid = getelementptr inbounds %struct.S, %struct.S* %src, i32 0, i32 1
      %dst_raw = bitcast i64* %dst_mid to i8*
      %src_raw = bitcast i64* %src_mid to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst_raw, i8* %src_raw, i64 16, i1 false)
      %after_copy = load i8*, i8** %dst_p
      ; Unknown-length memset at offset 8 must conservatively havoc offset 16!
      call void @llvm.memset.p0i8.i64(i8* %dst_raw, i8 0, i64 %n, i1 false)
      %after_memset = load i8*, i8** %dst_p
      ret i32 0
    }
  )IR");
  auto gOpt = options();
  gOpt.solver_mode = lotus::bootstrap::SolverMode::GuardedSummary;
  auto tOpt = options();
  tOpt.solver_mode = lotus::bootstrap::SolverMode::InputStateTabulation;

  BootstrapAA guarded(*f.module, nullptr, gOpt);
  BootstrapAA tab(*f.module, nullptr, tOpt);

  const auto b = guarded.objectId(*f.module->getNamedGlobal("b"));
  const auto &after_copy = f.instruction("main", "after_copy");
  const auto &after_memset = f.instruction("main", "after_memset");

  CHECK(guarded.pointsTo(after_copy, after_copy, {}, Point::After).points_to ==
        PointsToSet(b));
  CHECK(tab.pointsTo(after_copy, after_copy, {}, Point::After).points_to ==
        PointsToSet(b));
  CHECK(guarded.pointsTo(after_memset, after_memset, {}, Point::After)
            .points_to.isTop());
  CHECK(tab.pointsTo(after_memset, after_memset, {}, Point::After)
            .points_to.isTop());
}

void llvmPartialMemCopyInvalidatesPointerField() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.Pair = type { i8*, i8* }
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1 immarg)
    define i32 @main() {
      %src = alloca %struct.Pair
      %dst = alloca %struct.Pair
      %s0 = getelementptr inbounds %struct.Pair, %struct.Pair* %src, i32 0, i32 0
      %s1 = getelementptr inbounds %struct.Pair, %struct.Pair* %src, i32 0, i32 1
      %d0 = getelementptr inbounds %struct.Pair, %struct.Pair* %dst, i32 0, i32 0
      %d1 = getelementptr inbounds %struct.Pair, %struct.Pair* %dst, i32 0, i32 1
      store i8* @a, i8** %s0
      store i8* @b, i8** %s1
      store i8* @c, i8** %d1
      %ds = bitcast %struct.Pair* %dst to i8*
      %ss = bitcast %struct.Pair* %src to i8*
      ; 12 bytes copies f0 ([0,8)) completely and overwrites only half of f1 ([8,16))!
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %ds, i8* %ss, i64 12, i1 false)
      %r0 = load i8*, i8** %d0
      %r1 = load i8*, i8** %d1
      ret i32 0
    }
  )IR");
  auto gOpt = options();
  gOpt.solver_mode = lotus::bootstrap::SolverMode::GuardedSummary;
  auto tOpt = options();
  tOpt.solver_mode = lotus::bootstrap::SolverMode::InputStateTabulation;

  BootstrapAA guarded(*f.module, nullptr, gOpt);
  BootstrapAA tab(*f.module, nullptr, tOpt);

  const auto a = guarded.objectId(*f.module->getNamedGlobal("a"));
  const auto &r0 = f.instruction("main", "r0");
  const auto &r1 = f.instruction("main", "r1");

  CHECK(guarded.pointsTo(r0, r0, {}, Point::After).points_to == PointsToSet(a));
  CHECK(tab.pointsTo(r0, r0, {}, Point::After).points_to == PointsToSet(a));
  CHECK(guarded.pointsTo(r1, r1, {}, Point::After).points_to.isTop());
  CHECK(tab.pointsTo(r1, r1, {}, Point::After).points_to.isTop());
}

void llvmCallocInitializesNullObject() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.Node = type { i8*, i8* }
    @a = global i8 0
    declare i8* @calloc(i64, i64)
    define i32 @main(i1 %cond) {
    entry:
      %raw = call i8* @calloc(i64 1, i64 16)
      %n = bitcast i8* %raw to %struct.Node*
      %f0 = getelementptr inbounds %struct.Node, %struct.Node* %n, i32 0, i32 0
      %f1 = getelementptr inbounds %struct.Node, %struct.Node* %n, i32 0, i32 1
      br i1 %cond, label %init, label %join
    init:
      store i8* @a, i8** %f0
      br label %join
    join:
      %v0 = load i8*, i8** %f0
      %v1 = load i8*, i8** %f1
      ret i32 0
    }
  )IR");
  BootstrapAA aa(*f.module, nullptr, options());
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto &v0 = f.instruction("main", "v0");
  const auto &v1 = f.instruction("main", "v1");

  PointsToSet aOrNull(a);
  aOrNull.insert(lotus::bootstrap::NULL_OBJECT);
  CHECK(aa.pointsTo(v0, v0, {}, Point::After).points_to == aOrNull);
  CHECK(aa.pointsTo(v1, v1, {}, Point::After).points_to ==
        PointsToSet(lotus::bootstrap::NULL_OBJECT));
}

void llvmSubArrayInboundsGepAndEscapingHeapCastSoundness() {
  // Part A: Variable-index `inbounds` GEP on a sub-array inside a struct can
  // legally step into sibling fields in LLVM IR -> must include sibling `@b`.
  {
    Fixture f(R"IR(
      target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
      %struct.Mixed = type { [2 x i8*], i8* }
      @a = global i8 0
      @b = global i8 0
      define i32 @main(i64 %idx) {
        %s = alloca %struct.Mixed
        %e0 = getelementptr inbounds %struct.Mixed, %struct.Mixed* %s, i32 0, i32 0, i64 0
        %other = getelementptr inbounds %struct.Mixed, %struct.Mixed* %s, i32 0, i32 1
        store i8* @a, i8** %e0
        store i8* @b, i8** %other
        %p = getelementptr inbounds i8*, i8** %e0, i64 %idx
        %v = load i8*, i8** %p
        ret i32 0
      }
    )IR");
    BootstrapAA aa(*f.module, nullptr, options());
    const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
    const auto b = aa.objectId(*f.module->getNamedGlobal("b"));
    const auto &v = f.instruction("main", "v");
    auto res = aa.pointsTo(v, v, {}, Point::After);
    CHECK(res.reachable());
    CHECK(res.points_to.contains(a));
    CHECK(res.points_to.contains(b));
  }

  // Part B: Escaping raw `void*` from `malloc` passed to helper that casts to a
  // different struct layout inhibits unsound struct flattening.
  {
    Fixture f(R"IR(
      target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
      %struct.A = type { i32, i8* }
      %struct.B = type { i8*, i32 }
      @x = global i8 0
      declare i8* @malloc(i64)
      define void @init_as_B(i8* %raw) {
        %b = bitcast i8* %raw to %struct.B*
        %f0 = getelementptr inbounds %struct.B, %struct.B* %b, i32 0, i32 0
        store i8* @x, i8** %f0
        ret void
      }
      define i32 @main() {
        %raw = call i8* @malloc(i64 16)
        %a = bitcast i8* %raw to %struct.A*
        call void @init_as_B(i8* %raw)
        %as_ptr = bitcast %struct.A* %a to i8**
        %v = load i8*, i8** %as_ptr
        ret i32 0
      }
    )IR");
    BootstrapAA aa(*f.module, nullptr, options());
    const auto x = aa.objectId(*f.module->getNamedGlobal("x"));
    const auto &v = f.instruction("main", "v");
    auto res = aa.pointsTo(v, v, {}, Point::After);
    CHECK(res.reachable());
    CHECK(res.points_to.contains(x));
  }
}

void llvmStaleGlobalLoadPathSensitiveSoundness() {
  // Regression test for stale global-load canonicalization soundness:
  // `%v0 = load @g_flag` BEFORE `store 1, @g_flag` must NOT be canonicalized
  // to live `@g_flag` when branching on `%v0 == 0` after the store!
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    @g_flag = internal global i32 0
    @a = global i8 0
    @b = global i8 0
    define i32 @main() {
    entry:
      %slot = alloca i8*
      store i8* @b, i8** %slot
      %v0 = load i32, i32* @g_flag
      store i32 1, i32* @g_flag
      %c0 = icmp eq i32 %v0, 0
      br i1 %c0, label %then1, label %done
    then1:
      %v1 = load i32, i32* @g_flag
      %c1 = icmp ne i32 %v1, 0
      br i1 %c1, label %then2, label %done
    then2:
      store i8* @a, i8** %slot
      %res = load i8*, i8** %slot
      ret i32 0
    done:
      ret i32 0
    }
  )IR");

  for (bool interproc : {false, true}) {
    auto opt = options();
    opt.path_sensitive = true;
    opt.path_sensitive_interprocedural = interproc;
    BootstrapAA aa(*f.module, nullptr, opt);
    const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
    const auto &res = f.instruction("main", "res");
    auto qr = aa.pointsTo(res, res, {}, Point::After);
    CHECK(qr.reachable());
    CHECK(qr.points_to == PointsToSet(a));
  }
}

void llvmNullStoreNoBridgeAndNullAliasSemantics() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    @a = global i8 0
    @b = global i8 0
    define i32 @main(i1 %cond) {
    entry:
      %s1 = alloca i8*
      %s2 = alloca i8*
      store i8* null, i8** %s1
      store i8* null, i8** %s2
      br i1 %cond, label %write, label %done
    write:
      store i8* @a, i8** %s1
      store i8* @b, i8** %s2
      br label %done
    done:
      %r1 = load i8*, i8** %s1
      %r2 = load i8*, i8** %s2
      %n1 = alloca i8*
      %n2 = alloca i8*
      store i8* null, i8** %n1
      store i8* null, i8** %n2
      %ln1 = load i8*, i8** %n1
      %ln2 = load i8*, i8** %n2
      ret i32 0
    }
  )IR");

  const auto &r1 = f.instruction("main", "r1");
  const auto &r2 = f.instruction("main", "r2");
  const auto &ln1 = f.instruction("main", "ln1");
  const auto &ln2 = f.instruction("main", "ln2");
  const auto *ret_inst = f.module->getFunction("main")->back().getTerminator();

  for (auto mode : {lotus::bootstrap::SolverMode::GuardedSummary,
                    lotus::bootstrap::SolverMode::InputStateTabulation}) {
    auto opt = options();
    opt.solver_mode = mode;
    opt.andersen_threshold = 1000;
    BootstrapAA aa(*f.module, nullptr, opt);
    CHECK(aa.clustersFor(r1) != aa.clustersFor(r2));
    CHECK(!aa.mayAlias(ln1, ln2, *ret_inst));
    auto al = aa.aliases(ln1, *ret_inst);
    CHECK(al.reachable());
    CHECK(std::find(al.aliases.begin(), al.aliases.end(), &ln2) ==
          al.aliases.end());
  }
}

void llvmSwitchCompoundBoolSelectAndPtrGuards() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0
    define i32 @main(i32 %k, i1 %f1, i1 %f2) {
    entry:
      %sw_slot = alloca i8*
      store i8* @c, i8** %sw_slot
      switch i32 %k, label %sw_def [
        i32 0, label %sw0
        i32 1, label %sw1
      ]
    sw0:
      store i8* @a, i8** %sw_slot
      br label %sw_join
    sw1:
      store i8* @b, i8** %sw_slot
      br label %sw_join
    sw_def:
      br label %sw_join
    sw_join:
      %is0 = icmp eq i32 %k, 0
      br i1 %is0, label %check_sw0, label %after_sw
    check_sw0:
      %sw_res = load i8*, i8** %sw_slot
      br label %after_sw
    after_sw:
      ; Compound `and` + pointer `select`:
      %sel = select i1 %f1, i8* @a, i8* @b
      %both = and i1 %f1, %f2
      br i1 %both, label %sel_true, label %ptr_check
    sel_true:
      %sel_slot = alloca i8*
      store i8* %sel, i8** %sel_slot
      %sel_res = load i8*, i8** %sel_slot
      br label %ptr_check
    ptr_check:
      %p_slot = alloca i8*
      store i8* @a, i8** %p_slot
      %p = load i8*, i8** %p_slot
      %p_eq_b = icmp eq i8* %p, @b
      br i1 %p_eq_b, label %ptr_dead, label %ptr_live
    ptr_dead:
      %dead_ld = load i8*, i8** %p_slot
      br label %ptr_live
    ptr_live:
      ; Soundness: null == null must stay reachable!
      %ns1 = alloca i8*
      %ns2 = alloca i8*
      store i8* null, i8** %ns1
      store i8* null, i8** %ns2
      %np1 = load i8*, i8** %ns1
      %np2 = load i8*, i8** %ns2
      %null_eq = icmp eq i8* %np1, %np2
      br i1 %null_eq, label %null_eq_br, label %done
    null_eq_br:
      %null_ok = load i8*, i8** %p_slot
      br label %done
    done:
      ret i32 0
    }
  )IR");

  auto opt = options();
  opt.path_sensitive = true;
  BootstrapAA aa(*f.module, nullptr, opt);
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));

  const auto &sw_res = f.instruction("main", "sw_res");
  CHECK(aa.pointsTo(sw_res, sw_res, {}, Point::After).points_to ==
        PointsToSet(a));

  const auto &sel_res = f.instruction("main", "sel_res");
  CHECK(aa.pointsTo(sel_res, sel_res, {}, Point::After).points_to ==
        PointsToSet(a));

  const auto &dead_ld = f.instruction("main", "dead_ld");
  CHECK(aa.pointsTo(dead_ld, dead_ld, {}, Point::After).status ==
        lotus::bootstrap::QueryStatus::Unreachable);

  const auto &null_ok = f.instruction("main", "null_ok");
  auto nok = aa.pointsTo(null_ok, null_ok, {}, Point::After);
  CHECK(nok.reachable());
  CHECK(nok.points_to == PointsToSet(a));
}

void llvmMultiCallSiteInterproceduralCondSubst() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    @a = global i8 0
    @b = global i8 0
    define void @choose(i32 %mode, i8** %out) {
    entry:
      %c = icmp eq i32 %mode, 0
      br i1 %c, label %t, label %e
    t:
      store i8* @a, i8** %out
      ret void
    e:
      store i8* @b, i8** %out
      ret void
    }
    define i32 @main(i32 %x, i32 %y) {
    entry:
      %s1 = alloca i8*
      %s2 = alloca i8*
      %s3 = alloca i8*
      call void @choose(i32 0, i8** %s1)
      call void @choose(i32 1, i8** %s2)
      %r1 = load i8*, i8** %s1
      %r2 = load i8*, i8** %s2
      %cx = icmp eq i32 %x, 0
      br i1 %cx, label %call_var, label %done
    call_var:
      call void @choose(i32 %x, i8** %s3)
      %r3 = load i8*, i8** %s3
      br label %done
    done:
      ret i32 0
    }
  )IR");

  auto opt = options();
  opt.path_sensitive = true;
  opt.path_sensitive_interprocedural = true;
  BootstrapAA aa(*f.module, nullptr, opt);

  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto b = aa.objectId(*f.module->getNamedGlobal("b"));
  const auto &r1 = f.instruction("main", "r1");
  const auto &r2 = f.instruction("main", "r2");
  const auto &r3 = f.instruction("main", "r3");

  CHECK(aa.pointsTo(r1, r1, {}, Point::After).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(r2, r2, {}, Point::After).points_to == PointsToSet(b));
  CHECK(aa.pointsTo(r3, r3, {}, Point::After).points_to == PointsToSet(a));
}

void llvmObjectQueriesMustAliasSelectivePrecomputeAndHybrid() {
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    @g_wo = global i8* null
    @a = global i8 0
    @b = global i8 0
    declare i8* @malloc(i64)
    define i32 @main(i1 %cond) {
    entry:
      store i8* @a, i8** @g_wo
      %alias_a = bitcast i8* @a to i8*
      %h1 = call i8* @malloc(i64 8)
      %h1_copy = bitcast i8* %h1 to i8*
      %h2 = call i8* @malloc(i64 8)
      %other = alloca i8*
      store i8* @b, i8** %other
      %other_ld = load i8*, i8** %other
      %ret_anchor = alloca i8*
      br i1 %cond, label %t, label %e
    t:
      store i8* @a, i8** %ret_anchor
      br label %join
    e:
      store i8* @b, i8** %ret_anchor
      br label %join
    join:
      %weak_p = load i8*, i8** %ret_anchor
      %weak_q = load i8*, i8** %ret_anchor
      ret i32 0
    }
  )IR");

  const auto *g_wo = f.module->getNamedGlobal("g_wo");
  const auto &other_ld = f.instruction("main", "other_ld");
  const auto &h1 = f.instruction("main", "h1");
  const auto &h1_copy = f.instruction("main", "h1_copy");
  const auto &h2 = f.instruction("main", "h2");
  const auto &weak_p = f.instruction("main", "weak_p");
  const auto &weak_q = f.instruction("main", "weak_q");
  const auto *ret_inst = f.module->getFunction("main")->back().getTerminator();

  // Selective precomputation only solves requested cluster(s).
  {
    BootstrapAA sel(*f.module, nullptr, options());
    CHECK(sel.statistics().evaluated_clusters == 0);
    const auto other_clusters = sel.clustersFor(other_ld);
    CHECK(!other_clusters.empty());
    sel.precomputeForValues({&other_ld});
    CHECK(sel.statistics().evaluated_clusters == other_clusters.size());
    CHECK(sel.statistics().evaluated_clusters < sel.clusters().size());
  }

  // Hybrid solver + direct object queries + mustAlias.
  auto opt = options();
  opt.hybrid_tabulation_max_cluster_size = 2;
  BootstrapAA aa(*f.module, nullptr, opt);
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto g_id = aa.objectId(*g_wo);

  auto goPts = aa.objectPointsTo(g_id, *ret_inst);
  CHECK(goPts.reachable());
  CHECK(goPts.points_to == PointsToSet(a));
  auto goFsci = aa.objectPointsToAllContexts(g_id, *ret_inst);
  CHECK(goFsci.reachable());
  CHECK(goFsci.points_to == PointsToSet(a));
  auto goAl = aa.objectAliases(g_id, *ret_inst);
  CHECK(goAl.reachable());
  CHECK(!goAl.aliases.empty());

  CHECK(aa.mustAlias(h1, h1_copy, *ret_inst));
  CHECK(!aa.mustAlias(h1, h2, *ret_inst));
  CHECK(!aa.mustAlias(weak_p, weak_q, *ret_inst));
}

void llvmInteriorAndOnePastEndPointerComparisonAndMustAliasSoundness() {
  // Verify two subtle C/LLVM pointer-offset soundness cases:
  //  1. One-past-the-end pointer `(&g + 1) != &g` on a scalar singleton global
  //     must remain reachable under `--path-sensitive`, and `mustAlias` must be
  //     false.
  //  2. Interior sub-byte pointer `(char*)&s.f1 + 1 != (char*)&s.f1` inside a
  //     singleton struct field must remain reachable under `--path-sensitive`,
  //     and `mustAlias` must be false.
  Fixture f(R"IR(
    target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
    %struct.Pair = type { i8*, i8* }
    @g = global i8* null
    @a = global i8 0
    @b = global i8 0
    define i32 @main() {
    entry:
      %out = alloca i8*
      store i8* @b, i8** %out
      %end = getelementptr inbounds i8*, i8** @g, i64 1
      %cmp1 = icmp ne i8** @g, %end
      br i1 %cmp1, label %live1, label %after1
    live1:
      store i8* @a, i8** %out
      %r1 = load i8*, i8** %out
      br label %after1
    after1:
      %s = alloca %struct.Pair
      %f1 = getelementptr inbounds %struct.Pair, %struct.Pair* %s, i32 0, i32 1
      %f1_b = bitcast i8** %f1 to i8*
      %raw = bitcast %struct.Pair* %s to i8*
      %raw9 = getelementptr inbounds i8, i8* %raw, i64 9
      %cmp2 = icmp ne i8* %f1_b, %raw9
      br i1 %cmp2, label %live2, label %done
    live2:
      %r2 = load i8*, i8** %out
      br label %done
    done:
      ret i32 0
    }
  )IR");

  auto opt = options();
  opt.path_sensitive = true;
  BootstrapAA aa(*f.module, nullptr, opt);
  const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
  const auto *g = f.module->getNamedGlobal("g");
  const auto &end = f.instruction("main", "end");
  const auto &f1_b = f.instruction("main", "f1_b");
  const auto &raw9 = f.instruction("main", "raw9");
  const auto &r1 = f.instruction("main", "r1");
  const auto &r2 = f.instruction("main", "r2");
  const auto *ret_inst = f.module->getFunction("main")->back().getTerminator();

  auto qr1 = aa.pointsTo(r1, r1, {}, Point::After);
  CHECK(qr1.reachable());
  CHECK(qr1.points_to == PointsToSet(a));
  CHECK(!aa.mustAlias(*g, end, *ret_inst));

  auto qr2 = aa.pointsTo(r2, r2, {}, Point::After);
  CHECK(qr2.reachable());
  CHECK(qr2.points_to.contains(a));
  CHECK(!aa.mustAlias(f1_b, raw9, *ret_inst));
}

void llvmNestedArrayStrideWraparoundPartialZeroInitAndMemchrSoundness() {
  // Part A: Array of structs containing a sub-array `[4 x { [2 x i8*], i8* }]`:
  // a variable-index GEP on the inner `[2 x i8*]` (stride 8) does NOT have a
  // stride dividing/multiple of the outer period 24 (`8 % 24 != 0`), so it can
  // legally step into sibling field `+16` (`@c`) and must NOT drop `@c`.
  {
    Fixture f(R"IR(
      target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
      %Outer = type { [2 x i8*], i8* }
      @a = global i8 0
      @c = global i8 0
      @arr = global [4 x %Outer] zeroinitializer
      define i32 @main(i64 %i, i64 %j) {
        %p0 = getelementptr inbounds [4 x %Outer], [4 x %Outer]* @arr, i64 0, i64 0, i32 0, i64 0
        %p2 = getelementptr inbounds [4 x %Outer], [4 x %Outer]* @arr, i64 0, i64 0, i32 1
        store i8* @a, i8** %p0
        store i8* @c, i8** %p2
        %q = getelementptr inbounds [4 x %Outer], [4 x %Outer]* @arr, i64 0, i64 %i, i32 0, i64 %j
        %v = load i8*, i8** %q
        ret i32 0
      }
    )IR");
    BootstrapAA aa(*f.module, nullptr, options());
    const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
    const auto c = aa.objectId(*f.module->getNamedGlobal("c"));
    const auto &v = f.instruction("main", "v");
    auto qr = aa.pointsTo(v, v, {}, Point::After);
    CHECK(qr.reachable());
    CHECK(qr.points_to.contains(a));
    CHECK(qr.points_to.contains(c));
  }

  // Part B: `memset` starting at `&arr[0].b` (offset 8) with length 16 wraps
  // across array stride 16 into `arr[1].a` (folded offset 0), so `arr[1].a`
  // must be invalidated to Top!
  {
    Fixture f(R"IR(
      target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
      %Pair = type { i8*, i8* }
      @a = global i8 0
      @b = global i8 0
      declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1 immarg)
      define i32 @main() {
        %arr = alloca [2 x %Pair]
        %a10 = getelementptr inbounds [2 x %Pair], [2 x %Pair]* %arr, i64 0, i64 1, i32 0
        %a01 = getelementptr inbounds [2 x %Pair], [2 x %Pair]* %arr, i64 0, i64 0, i32 1
        store i8* @a, i8** %a10
        store i8* @b, i8** %a01
        %raw = bitcast i8** %a01 to i8*
        call void @llvm.memset.p0i8.i64(i8* %raw, i8 0, i64 16, i1 false)
        %v = load i8*, i8** %a10
        ret i32 0
      }
    )IR");
    for (auto mode : {lotus::bootstrap::SolverMode::GuardedSummary,
                      lotus::bootstrap::SolverMode::InputStateTabulation}) {
      auto opt = options();
      opt.solver_mode = mode;
      BootstrapAA aa(*f.module, nullptr, opt);
      const auto &v = f.instruction("main", "v");
      auto qr = aa.pointsTo(v, v, {}, Point::After);
      CHECK(qr.reachable());
      CHECK(qr.points_to.isTop());
    }
  }

  // Part C: Partially zero-initialized and undef-initialized global arrays of
  // structs must fold element 1's `zeroinitializer` (`NULL_OBJECT`) and `undef`
  // (`Top`) onto the folded fields at offsets 0 and 8.
  {
    Fixture f(R"IR(
      target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
      %Pair = type { i8*, i8* }
      @a = global i8 0
      @b = global i8 0
      @arr_zero = global [2 x %Pair] [ %Pair { i8* @a, i8* @b }, %Pair zeroinitializer ]
      @arr_undef = global [2 x %Pair] [ %Pair { i8* @a, i8* @b }, %Pair undef ]
      define i32 @main() {
      entry:
        %pz = getelementptr inbounds [2 x %Pair], [2 x %Pair]* @arr_zero, i64 0, i64 1, i32 0
        %vz = load i8*, i8** %pz
        %is_null = icmp eq i8* %vz, null
        br i1 %is_null, label %null_ok, label %done
      null_ok:
        %reached = bitcast i8* @a to i8*
        br label %done
      done:
        %pu = getelementptr inbounds [2 x %Pair], [2 x %Pair]* @arr_undef, i64 0, i64 1, i32 0
        %vu = load i8*, i8** %pu
        ret i32 0
      }
    )IR");
    auto opt = options();
    opt.path_sensitive = true;
    BootstrapAA aa(*f.module, nullptr, opt);
    const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
    const auto &vz = f.instruction("main", "vz");
    const auto &reached = f.instruction("main", "reached");
    const auto &vu = f.instruction("main", "vu");
    auto qz = aa.pointsTo(vz, vz, {}, Point::After);
    CHECK(qz.reachable());
    CHECK(qz.points_to.contains(a));
    CHECK(qz.points_to.contains(lotus::bootstrap::NULL_OBJECT));
    auto qr = aa.pointsTo(reached, reached, {}, Point::After);
    CHECK(qr.reachable());
    CHECK(qr.points_to == PointsToSet(a));
    auto qu = aa.pointsTo(vu, vu, {}, Point::After);
    CHECK(qu.reachable());
    CHECK(qu.points_to.isTop());
  }

  // Part D: `memchr` returns an interior pointer into any field of a flattened
  // struct (including field 1!) and must not `mustAlias` the base singleton.
  {
    Fixture f(R"IR(
      target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
      %Pair = type { i8*, i8* }
      @a = global i8 0
      @b = global i8 0
      declare i8* @memchr(i8*, i32, i64)
      define i32 @main() {
        %s = alloca %Pair
        %f0 = getelementptr inbounds %Pair, %Pair* %s, i32 0, i32 0
        %f1 = getelementptr inbounds %Pair, %Pair* %s, i32 0, i32 1
        store i8* @a, i8** %f0
        store i8* @b, i8** %f1
        %raw = bitcast %Pair* %s to i8*
        %hit = call i8* @memchr(i8* %raw, i32 0, i64 16)
        %hit_pp = bitcast i8* %hit to i8**
        %val = load i8*, i8** %hit_pp
        ret i32 0
      }
    )IR");
    BootstrapAA aa(*f.module, nullptr, options());
    const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
    const auto b = aa.objectId(*f.module->getNamedGlobal("b"));
    const auto &raw = f.instruction("main", "raw");
    const auto &hit = f.instruction("main", "hit");
    const auto &val = f.instruction("main", "val");
    const auto *ret_inst = f.module->getFunction("main")->back().getTerminator();
    auto qv = aa.pointsTo(val, val, {}, Point::After);
    CHECK(qv.reachable());
    CHECK(qv.points_to.contains(a));
    CHECK(qv.points_to.contains(b));
    CHECK(!aa.mustAlias(raw, hit, *ret_inst));
  }

  // Part E: `byval` argument receives a fresh stack copy whose address is
  // distinct from the caller's actual `@g`, so `callee`'s `arg != @g` branch
  // must stay reachable under `path_sensitive_interprocedural`.
  {
    Fixture f(R"IR(
      target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
      @g = global i8 0
      @a = global i8 0
      define i8* @callee(i8* byval(i8) %arg) {
      entry:
        %same = icmp eq i8* %arg, @g
        br i1 %same, label %yes, label %no
      yes:
        ret i8* null
      no:
        %ok = bitcast i8* @a to i8*
        ret i8* %ok
      }
      define i32 @main() {
        %res = call i8* @callee(i8* byval(i8) @g)
        ret i32 0
      }
    )IR");
    auto opt = options();
    opt.path_sensitive = true;
    opt.path_sensitive_interprocedural = true;
    BootstrapAA aa(*f.module, nullptr, opt);
    const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
    const auto &res = f.instruction("main", "res");
    auto qr = aa.pointsTo(res, res, {}, Point::After);
    CHECK(qr.reachable());
    CHECK(qr.points_to.contains(a));
  }

  // Part F: Constant-offset pointer arithmetic stepping past the last element
  // of a folded sub-array (`&g[0].arr[1] + 1 == &g[0].tail`), BOTH directly
  // and after round-tripping the sub-array pointer through memory, must include
  // the sibling struct field `&g[0].tail` in GuardedSummary and Tabulation!
  {
    Fixture f(R"IR(
      target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
      %Outer = type { [2 x i8*], i8* }
      @x = global i8 0
      @y = global i8 0
      @g = global [2 x %Outer] zeroinitializer
      define i32 @main() {
        %slot = alloca i8**
        %p_arr1 = getelementptr inbounds [2 x %Outer], [2 x %Outer]* @g, i64 0, i64 0, i32 0, i64 1
        %q_tail = getelementptr inbounds i8*, i8** %p_arr1, i64 1
        store i8* @x, i8** %q_tail
        store i8** %p_arr1, i8*** %slot
        %loaded = load i8**, i8*** %slot
        %q_tail2 = getelementptr inbounds i8*, i8** %loaded, i64 1
        store i8* @y, i8** %q_tail2
        %tail_ptr = getelementptr inbounds [2 x %Outer], [2 x %Outer]* @g, i64 0, i64 0, i32 1
        %r = load i8*, i8** %tail_ptr
        ret i32 0
      }
    )IR");
    for (auto mode : {lotus::bootstrap::SolverMode::GuardedSummary,
                      lotus::bootstrap::SolverMode::InputStateTabulation}) {
      auto opt = options();
      opt.solver_mode = mode;
      BootstrapAA aa(*f.module, nullptr, opt);
      const auto x = aa.objectId(*f.module->getNamedGlobal("x"));
      const auto y = aa.objectId(*f.module->getNamedGlobal("y"));
      const auto &r = f.instruction("main", "r");
      auto qr = aa.pointsTo(r, r, {}, Point::After);
      CHECK(qr.reachable());
      CHECK(qr.points_to.contains(x));
      CHECK(qr.points_to.contains(y));
    }
  }

  // Part G: Multi-stride `memcpy` into a folded array of structs
  // (`memcpy(&dst[0], &src_four, 32)` where `dst` is `[2 x %Pair]`) must copy
  // BOTH element 0 (`@a`, `@b`) AND element 1 (`@c`, `@d`) into folded fields
  // `dst+0` and `dst+8`; and a partial second-element copy (`20` bytes) must
  // invalidate `dst+0` to `Top` across all backends (including OLF).
  {
    Fixture f(R"IR(
      target datalayout = "e-m:e-i64:64-n8:16:32:64-S128"
      %Pair = type { i8*, i8* }
      %Four = type { i8*, i8*, i8*, i8* }
      @a = global i8 0
      @b = global i8 0
      @c = global i8 0
      @d = global i8 0
      declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1 immarg)
      define i32 @main() {
        %src = alloca %Four
        %dst = alloca [2 x %Pair]
        %dst_part = alloca [2 x %Pair]
        %s0 = getelementptr inbounds %Four, %Four* %src, i32 0, i32 0
        %s1 = getelementptr inbounds %Four, %Four* %src, i32 0, i32 1
        %s2 = getelementptr inbounds %Four, %Four* %src, i32 0, i32 2
        %s3 = getelementptr inbounds %Four, %Four* %src, i32 0, i32 3
        store i8* @a, i8** %s0
        store i8* @b, i8** %s1
        store i8* @c, i8** %s2
        store i8* @d, i8** %s3
        %src_raw = bitcast %Four* %src to i8*
        %dst_raw = bitcast [2 x %Pair]* %dst to i8*
        call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst_raw, i8* %src_raw, i64 32, i1 false)
        %d10 = getelementptr inbounds [2 x %Pair], [2 x %Pair]* %dst, i64 0, i64 1, i32 0
        %d11 = getelementptr inbounds [2 x %Pair], [2 x %Pair]* %dst, i64 0, i64 1, i32 1
        %v10 = load i8*, i8** %d10
        %v11 = load i8*, i8** %d11
        %dp_raw = bitcast [2 x %Pair]* %dst_part to i8*
        call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dp_raw, i8* %src_raw, i64 20, i1 false)
        %dp10 = getelementptr inbounds [2 x %Pair], [2 x %Pair]* %dst_part, i64 0, i64 1, i32 0
        %vp10 = load i8*, i8** %dp10
        ret i32 0
      }
    )IR");
    for (auto mode : {lotus::bootstrap::SolverMode::GuardedSummary,
                      lotus::bootstrap::SolverMode::InputStateTabulation}) {
      for (bool olf : {false, true}) {
        if (olf && mode == lotus::bootstrap::SolverMode::InputStateTabulation)
          continue;
        auto opt = options();
        opt.solver_mode = mode;
        opt.enable_one_level_flow = olf;
        opt.andersen_threshold = 2;
        BootstrapAA aa(*f.module, nullptr, opt);
        const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
        const auto b = aa.objectId(*f.module->getNamedGlobal("b"));
        const auto c = aa.objectId(*f.module->getNamedGlobal("c"));
        const auto d = aa.objectId(*f.module->getNamedGlobal("d"));
        const auto &v10 = f.instruction("main", "v10");
        const auto &v11 = f.instruction("main", "v11");
        const auto &vp10 = f.instruction("main", "vp10");
        auto q10 = aa.pointsTo(v10, v10, {}, Point::After);
        auto q11 = aa.pointsTo(v11, v11, {}, Point::After);
        auto qp10 = aa.pointsTo(vp10, vp10, {}, Point::After);
        CHECK(q10.reachable());
        CHECK(q10.points_to.contains(a));
        CHECK(q10.points_to.contains(c));
        CHECK(q11.reachable());
        CHECK(q11.points_to.contains(b));
        CHECK(q11.points_to.contains(d));
        CHECK(qp10.reachable());
        CHECK(qp10.points_to.isTop());
      }
    }
  }
}

void llvmPackedStructCrossFieldStoreBitcastBoolAndDisjointIndirectCall() {
  // Part A: Packed struct `<{ i32, i8* }>` overwritten via bitcast 8-byte
  // pointer store at offset 0 overlaps sibling pointer field at offset 4, so
  // loading field 1 must soundly widen to Top across all solver backends.
  {
    Fixture f(R"IR(
      target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
      %Packed = type <{ i32, i8* }>
      @a = global i8 0
      @b = global i8 0
      define i32 @main() {
        %s = alloca %Packed
        %f1 = getelementptr inbounds %Packed, %Packed* %s, i32 0, i32 1
        store i8* @a, i8** %f1
        %raw = bitcast %Packed* %s to i8**
        store i8* @b, i8** %raw
        %v = load i8*, i8** %f1
        ret i32 0
      }
    )IR");
    for (int m = 0; m < 4; ++m) {
      auto opt = options();
      if (m == 1)
        opt.solver_mode = lotus::bootstrap::SolverMode::InputStateTabulation;
      else if (m == 2)
        opt.enable_one_level_flow = true;
      else if (m == 3)
        opt.path_sensitive = true;
      BootstrapAA aa(*f.module, nullptr, opt);
      const auto &v = f.instruction("main", "v");
      auto qr = aa.pointsTo(v, v, {}, Point::After);
      CHECK(qr.reachable());
      CHECK(qr.points_to.isTop());
    }
  }

  // Part B: Bitcast call passing non-i1 constant `i32 2` (low bit 0!) to an
  // `i1` parameter must not wrongly fold `!ci->isZero()` to `i1 true` under
  // `path_sensitive_interprocedural`, and disjoint-cluster indirect calls must
  // keep subsequent loads reachable across Tabulation and Hybrid modes.
  {
    Fixture f(R"IR(
      target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
      @a0 = global i32 0
      @a1 = global i32 1
      @t0 = global i32 2
      @g_p = global i32* @t0
      @g_pp = global i32** @g_p
      @g_fn = global i32* ()* @leaf
      define i32* @leaf() {
        ret i32* @a0
      }
      define void @helper(i1 %flag, i32** %out, i32* %x, i32* %y) {
      entry:
        br i1 %flag, label %t, label %f
      t:
        store i32* %x, i32** %out
        ret void
      f:
        store i32* %y, i32** %out
        ret void
      }
      define i32 @main() {
        %slot = alloca i32*
        call void bitcast (void (i1, i32**, i32*, i32*)* @helper to void (i32, i32**, i32*, i32*)*)(i32 2, i32** %slot, i32* @a0, i32* @a1)
        %h = load i32*, i32** %slot
        %fp = load i32* ()*, i32* ()** @g_fn
        %ind = call i32* %fp()
        %l1 = load i32**, i32*** @g_pp
        %l2 = load i32*, i32** %l1
        ret i32 0
      }
    )IR");
    for (int m = 0; m < 4; ++m) {
      auto opt = options();
      if (m == 0) {
        opt.path_sensitive = true;
        opt.path_sensitive_interprocedural = true;
      } else if (m == 1) {
        opt.solver_mode = lotus::bootstrap::SolverMode::InputStateTabulation;
      } else if (m == 2) {
        opt.hybrid_tabulation_max_cluster_size = 6;
      } else {
        opt.enable_one_level_flow = true;
      }
      BootstrapAA aa(*f.module, nullptr, opt);
      const auto a1 = aa.objectId(*f.module->getNamedGlobal("a1"));
      const auto g_p = aa.objectId(*f.module->getNamedGlobal("g_p"));
      const auto t0 = aa.objectId(*f.module->getNamedGlobal("t0"));
      const auto &h = f.instruction("main", "h");
      const auto &l1 = f.instruction("main", "l1");
      const auto &l2 = f.instruction("main", "l2");
      auto qh = aa.pointsTo(h, h, {}, Point::After);
      CHECK(qh.reachable());
      CHECK(qh.points_to.contains(a1));
      auto q1 = aa.pointsTo(l1, l1, {}, Point::After);
      CHECK(q1.reachable());
      CHECK(q1.points_to == PointsToSet(g_p));
      auto q2 = aa.pointsTo(l2, l2, {}, Point::After);
      CHECK(q2.reachable());
      CHECK(q2.points_to == PointsToSet(t0));
    }
  }

  // Part C: Volatile loads of a local global must NOT be correlated as equal
  // under path sensitivity, and a dead caller invoking `main(0, null)` must
  // NOT collapse `main`'s entry parameter `%argc` to `0`.
  {
    Fixture f(R"IR(
      target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
      @g_vol = internal global i32 0
      @a = global i32 0
      @b = global i32 1
      define void @dead_caller() {
        %r = call i32 @main(i32 0, i8** null)
        ret void
      }
      define i32 @main(i32 %argc, i8** %argv) {
      entry:
        %slot = alloca i32*
        store i32* @a, i32** %slot
        %v1 = load volatile i32, i32* @g_vol
        %c1 = icmp eq i32 %v1, 1
        br i1 %c1, label %t1, label %after1
      t1:
        %v2 = load volatile i32, i32* @g_vol
        %c2 = icmp ne i32 %v2, 1
        br i1 %c2, label %both, label %after1
      both:
        store i32* @b, i32** %slot
        br label %after1
      after1:
        %q_vol = load i32*, i32** %slot
        %ac = icmp eq i32 %argc, 0
        br i1 %ac, label %argc0, label %argc_nz
      argc0:
        %p0 = bitcast i32* @a to i32*
        br label %done
      argc_nz:
        %p1 = bitcast i32* @b to i32*
        br label %done
      done:
        %q_argc = phi i32* [ %p0, %argc0 ], [ %p1, %argc_nz ]
        ret i32 0
      }
    )IR");
    auto opt = options();
    opt.path_sensitive = true;
    opt.path_sensitive_interprocedural = true;
    BootstrapAA aa(*f.module, nullptr, opt);
    const auto a = aa.objectId(*f.module->getNamedGlobal("a"));
    const auto b = aa.objectId(*f.module->getNamedGlobal("b"));
    const auto &q_vol = f.instruction("main", "q_vol");
    const auto &q_argc = f.instruction("main", "q_argc");
    auto qv = aa.pointsTo(q_vol, q_vol, {}, Point::After);
    CHECK(qv.reachable());
    CHECK(qv.points_to.contains(a));
    CHECK(qv.points_to.contains(b));
    auto qa = aa.pointsTo(q_argc, q_argc, {}, Point::After);
    CHECK(qa.reachable());
    CHECK(qa.points_to.contains(a));
    CHECK(qa.points_to.contains(b));
  }
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
TEST(BootstrapAATest, LLVMCalleeInLoopGuardSoundness) {
  llvmCalleeInLoopGuardSoundness();
}
TEST(BootstrapAATest, LLVMInterproceduralUpwardGlobalFlagMutationSoundness) {
  llvmInterproceduralUpwardGlobalFlagMutationSoundness();
}
TEST(BootstrapAATest, LLVMNonZeroOffsetMemCopyAndUnknownLenMemsetSliceSoundness) {
  llvmNonZeroOffsetMemCopyAndUnknownLenMemsetSliceSoundness();
}
TEST(BootstrapAATest, LLVMPartialMemCopyInvalidatesPointerField) {
  llvmPartialMemCopyInvalidatesPointerField();
}
TEST(BootstrapAATest, LLVMCallocInitializesNullObject) {
  llvmCallocInitializesNullObject();
}
TEST(BootstrapAATest, LLVMSubArrayInboundsGepAndEscapingHeapCastSoundness) {
  llvmSubArrayInboundsGepAndEscapingHeapCastSoundness();
}
TEST(BootstrapAATest, LLVMStaleGlobalLoadPathSensitiveSoundness) {
  llvmStaleGlobalLoadPathSensitiveSoundness();
}
TEST(BootstrapAATest, LLVMNullStoreNoBridgeAndNullAliasSemantics) {
  llvmNullStoreNoBridgeAndNullAliasSemantics();
}
TEST(BootstrapAATest, LLVMSwitchCompoundBoolSelectAndPtrGuards) {
  llvmSwitchCompoundBoolSelectAndPtrGuards();
}
TEST(BootstrapAATest, LLVMMultiCallSiteInterproceduralCondSubst) {
  llvmMultiCallSiteInterproceduralCondSubst();
}
TEST(BootstrapAATest, LLVMObjectQueriesMustAliasSelectivePrecomputeAndHybrid) {
  llvmObjectQueriesMustAliasSelectivePrecomputeAndHybrid();
}
TEST(BootstrapAATest,
     LLVMInteriorAndOnePastEndPointerComparisonAndMustAliasSoundness) {
  llvmInteriorAndOnePastEndPointerComparisonAndMustAliasSoundness();
}
TEST(BootstrapAATest,
     LLVMNestedArrayStrideWraparoundPartialZeroInitAndMemchrSoundness) {
  llvmNestedArrayStrideWraparoundPartialZeroInitAndMemchrSoundness();
}
TEST(BootstrapAATest,
     LLVMPackedStructCrossFieldStoreBitcastBoolAndDisjointIndirectCall) {
  llvmPackedStructCrossFieldStoreBitcastBoolAndDisjointIndirectCall();
}
