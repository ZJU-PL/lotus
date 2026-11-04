#include "Verification/TraceAbstraction/TraceAbstractionEngine.h"
#include "Verification/TraceAbstraction/TraceChecker.h"
#include "Verification/TraceAbstraction/TransitionFormula.h"

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>

using namespace lotus::verification::traceabstraction;
class TraceAbstractionMemoryTest : public ::testing::Test {
protected:
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  llvm::Function &parse(const std::string &ir) {
    llvm::SMDiagnostic diagnostic;
    module = llvm::parseAssemblyString(ir, diagnostic, context);
    if (!module) {
      diagnostic.print("memory-test", llvm::errs());
      throw std::runtime_error("invalid test IR");
    }
    if (llvm::verifyModule(*module, &llvm::errs()))
      throw std::runtime_error("invalid LLVM module");
    return *module->getFunction("main");
  }
  VerificationResult verify(const std::string &ir) {
    auto &f = parse(ir);
    TraceAbstractionOptions options;
    options.timeout = std::chrono::milliseconds(30000);
    return TraceAbstractionEngine(options).verify(f);
  }
};

TEST_F(TraceAbstractionMemoryTest, ArrayAliasAndPointerArgument) {
  auto result = verify(R"(
    declare void @abort()
    define void @inc(i32* %p) {
      %x = load i32, i32* %p
      %y = add i32 %x, 1
      store i32 %y, i32* %p
      ret void
    }
    define void @main() {
      %a = alloca [2 x i32]
      %p = getelementptr inbounds [2 x i32], [2 x i32]* %a, i32 0, i32 1
      store i32 1, i32* %p
      call void @inc(i32* %p)
      %alias = getelementptr [2 x i32], [2 x i32]* %a, i32 0, i32 1
      %v = load i32, i32* %alias
      %ok = icmp eq i32 %v, 2
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest,
       GlobalAggregateInitializerAndSharedMutation) {
  auto result = verify(R"(
    %Pair = type { i8, i32 }
    @g = global %Pair { i8 7, i32 41 }
    declare void @abort()
    define void @update() {
      %p = getelementptr %Pair, %Pair* @g, i32 0, i32 1
      %x = load i32, i32* %p
      %next = add i32 %x, 1
      store i32 %next, i32* %p
      ret void
    }
    define void @main() {
      call void @update()
      %v = load i32, i32* getelementptr (%Pair, %Pair* @g, i32 0, i32 1)
      %ok = icmp eq i32 %v, 42
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest, ByteStoresComposeAndRespectEndianness) {
  for (const auto &layout :
       {std::string("e-p:32:32"), std::string("E-p:64:64")}) {
    auto result = verify("target datalayout = \"" + layout + "\"\n" + R"(
      declare void @abort()
      define void @main() {
        %p = alloca i32
        store i32 287454020, i32* %p
        %b = bitcast i32* %p to i8*
        %second = getelementptr i8, i8* %b, i32 1
        store i8 0, i8* %second
        %v = load i32, i32* %p
        %ok = icmp eq i32 %v, )" +
                         (layout[0] == 'e' ? "287440964" : "285225796") + R"(
        br i1 %ok, label %done, label %error
      error: call void @abort()
        unreachable
      done: ret void
      })");
    EXPECT_EQ(result.status, VerificationResult::Safe)
        << layout << ": " << result.reason;
  }
}

TEST_F(TraceAbstractionMemoryTest, PointerStoredInStructRoundTrips) {
  auto result = verify(R"(
    %Box = type { i8, i32* }
    declare void @abort()
    define void @main() {
      %x = alloca i32
      store i32 12, i32* %x
      %box = alloca %Box
      %field = getelementptr %Box, %Box* %box, i32 0, i32 1
      store i32* %x, i32** %field
      %p = load i32*, i32** %field
      store i32 13, i32* %p
      %v = load i32, i32* %x
      %ok = icmp eq i32 %v, 13
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest,
       BranchInitializationDoesNotNeedOneDominatingStore) {
  auto result = verify(R"(
    declare void @abort()
    define void @main(i1 %choose) {
      %p = alloca i32
      br i1 %choose, label %left, label %right
    left: store i32 1, i32* %p
      br label %join
    right: store i32 2, i32* %p
      br label %join
    join:
      %v = load i32, i32* %p
      %bad = icmp eq i32 %v, 3
      br i1 %bad, label %error, label %done
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest, HeapAllocationAndFree) {
  auto result = verify(R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare void @abort()
    define void @main() {
      %raw = call i8* @malloc(i64 8)
      %failed = icmp eq i8* %raw, null
      br i1 %failed, label %done, label %body
    body:
      %p = bitcast i8* %raw to i32*
      store i32 9, i32* %p
      %v = load i32, i32* %p
      call void @free(i8* %raw)
      %ok = icmp eq i32 %v, 9
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest, MemmoveUsesPreStateForOverlappingCopy) {
  auto result = verify(R"(
    declare void @llvm.memmove.p0i8.p0i8.i64(i8* nocapture writeonly, i8* nocapture readonly, i64, i1 immarg)
    declare void @abort()
    define void @main() {
      %a = alloca [3 x i8]
      %p = getelementptr [3 x i8], [3 x i8]* %a, i32 0, i32 0
      %q = getelementptr i8, i8* %p, i32 1
      %r = getelementptr i8, i8* %p, i32 2
      store i8 1, i8* %p
      store i8 2, i8* %q
      call void @llvm.memmove.p0i8.p0i8.i64(i8* %q, i8* %p, i64 2, i1 false)
      %v = load i8, i8* %r
      %ok = icmp eq i8 %v, 2
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest, LifetimeRestartAndInvalidAccess) {
  auto result = verify(R"(
    declare void @llvm.lifetime.start.p0i8(i64 immarg, i8* nocapture)
    declare void @llvm.lifetime.end.p0i8(i64 immarg, i8* nocapture)
    declare void @abort()
    define void @main() {
      %p = alloca i8
      call void @llvm.lifetime.start.p0i8(i64 1, i8* %p)
      store i8 7, i8* %p
      call void @llvm.lifetime.end.p0i8(i64 1, i8* %p)
      call void @llvm.lifetime.start.p0i8(i64 1, i8* %p)
      store i8 8, i8* %p
      %v = load i8, i8* %p
      %ok = icmp eq i8 %v, 8
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
  result = verify(R"(
    declare void @abort()
    define void @main() {
      %p = alloca i32
      %q = getelementptr i32, i32* %p, i32 1
      store i32 4, i32* %q
      call void @abort()
      unreachable
    })");
  EXPECT_EQ(result.status, VerificationResult::Unknown) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest, RecursiveCallerLocalsAreRestored) {
  auto result = verify(R"(
    declare void @abort()
    define i32 @sum(i32 %n) {
      %stop = icmp eq i32 %n, 0
      br i1 %stop, label %base, label %step
    base: ret i32 0
    step:
      %next = sub i32 %n, 1
      %tail = call i32 @sum(i32 %next)
      %value = add i32 %tail, %n
      ret i32 %value
    }
    define void @main() {
      %v = call i32 @sum(i32 3)
      %ok = icmp eq i32 %v, 6
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest,
       RecursiveFramesKeepCallerObjectsAndHeapEffects) {
  auto result = verify(R"(
    @g = global i32 0
    declare void @abort()
    define i32 @rec(i32 %n, i32* %p) {
      %local = alloca i32
      store i32 %n, i32* %local
      %stop = icmp eq i32 %n, 0
      br i1 %stop, label %done, label %step
    step:
      %old = load i32, i32* %p
      %new = add i32 %old, 1
      store i32 %new, i32* %p
      %next = sub i32 %n, 1
      %ignored = call i32 @rec(i32 %next, i32* %p)
      br label %done
    done:
      %v = load i32, i32* %local
      ret i32 %v
    }
    define void @main() {
      %r = call i32 @rec(i32 2, i32* @g)
      %v = load i32, i32* @g
      %a = icmp eq i32 %r, 2
      %b = icmp eq i32 %v, 2
      %ok = and i1 %a, %b
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest,
       ReturnHoareCheckNeedsTheHierarchicalPredicate) {
  auto &f = parse(R"(
    declare void @abort()
    define void @callee() { ret void }
    define void @main(i32 %x) {
      call void @callee()
      call void @abort()
      unreachable
    })");
  auto p = IcfgToNwa::build(f);
  TraceChecker checker(p);
  auto &semantics = checker.semantics();
  auto &ctx = *semantics.context();
  unsigned x = 0;
  for (; x < semantics.variables().size(); ++x)
    if (semantics.variables()[x].value == f.getArg(0))
      break;
  ASSERT_LT(x, semantics.variables().size());
  auto predicate = semantics.inputs()[x] == ctx.int_val(7);
  for (auto letter : p.automaton->alphabet().letters(nwa::LetterKind::Return)) {
    EXPECT_TRUE(checker.isReturnInductive(ctx.bool_val(true), predicate, letter,
                                          predicate));
    EXPECT_FALSE(checker.isReturnInductive(
        ctx.bool_val(true), ctx.bool_val(true), letter, predicate));
    EXPECT_FALSE(checker.isInductive(predicate, letter, predicate));
  }
}

TEST_F(TraceAbstractionMemoryTest, DivisionShiftsAndFreeze) {
  auto result = verify(R"(
    declare void @abort()
    define void @main() {
      %f = freeze i8 undef
      %stable = icmp eq i8 %f, %f
      %a = udiv exact i8 12, 3
      %b = shl nuw i8 %a, 2
      %c = lshr exact i8 %b, 1
      %d = srem i8 -9, 2
      %ok1 = icmp eq i8 %c, 8
      %ok2 = icmp eq i8 %d, -1
      %both = and i1 %ok1, %ok2
      %ok = and i1 %both, %stable
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest, UnboundedRecursiveReturnProperty) {
  auto result = verify(R"(
    declare void @abort()
    define i32 @rec(i32 %n) {
      %stop = icmp eq i32 %n, 0
      br i1 %stop, label %base, label %step
    base: ret i32 0
    step:
      %next = sub i32 %n, 1
      %r = call i32 @rec(i32 %next)
      ret i32 %r
    }
    define void @main(i32 %n) {
      %r = call i32 @rec(i32 %n)
      %ok = icmp eq i32 %r, 0
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
  EXPECT_LE(result.iterations, 6u);
}

TEST_F(TraceAbstractionMemoryTest, SharedMemoryBugSurvivesReturn) {
  auto result = verify(R"(
    @g = global i32 0
    declare void @abort()
    define void @change(i32* %p) {
      store i32 1, i32* %p
      ret void
    }
    define void @main() {
      call void @change(i32* @g)
      %v = load i32, i32* @g
      %ok = icmp eq i32 %v, 0
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Unsafe) << result.reason;
  EXPECT_FALSE(result.assignments.empty());
}

TEST_F(TraceAbstractionMemoryTest, ReturnedStackAddressIsNoLongerLive) {
  auto result = verify(R"(
    declare void @abort()
    define i32* @escape() {
      %p = alloca i32
      store i32 7, i32* %p
      ret i32* %p
    }
    define void @main() {
      %p = call i32* @escape()
      %v = load i32, i32* %p
      call void @abort()
      unreachable
    })");
  EXPECT_EQ(result.status, VerificationResult::Unknown) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest, SignedOneBitExtension) {
  auto result = verify(R"(
    declare void @abort()
    define void @main() {
      %x = sext i1 true to i8
      %negative = icmp slt i1 true, false
      %minusone = icmp eq i8 %x, -1
      %ok = and i1 %negative, %minusone
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionMemoryTest,
       SymbolicNonlinearArithmeticIsExplicitlyUnsupported) {
  auto result = verify(R"(
    declare void @abort()
    define void @main(i32 %x, i32 %y) {
      %r = mul i32 %x, %y
      call void @abort()
      unreachable
    })");
  EXPECT_EQ(result.status, VerificationResult::Unknown);
  EXPECT_NE(result.reason.find("linear arithmetic"), std::string::npos);
}

namespace {
bool hasBitVector(const z3::sort &s) {
  if (s.is_bv())
    return true;
  if (s.is_array())
    return hasBitVector(s.array_domain()) || hasBitVector(s.array_range());
  if (s.is_datatype()) {
    for (unsigned c = 0; c < Z3_get_datatype_sort_num_constructors(s.ctx(), s);
         ++c) {
      auto ctor = Z3_get_datatype_sort_constructor(s.ctx(), s, c);
      for (unsigned j = 0; j < Z3_get_domain_size(s.ctx(), ctor); ++j)
        if (hasBitVector(z3::sort(s.ctx(), Z3_get_domain(s.ctx(), ctor, j))))
          return true;
    }
  }
  return false;
}
} // namespace
TEST_F(TraceAbstractionMemoryTest, EntireVocabularyAndFormulasUseNoBitVectors) {
  auto &f = parse(R"(
    @g = global i8 7
    declare void @abort()
    define void @main(i8 %x) {
      %p = alloca i8
      %v = add i8 %x, 1
      store i8 %v, i8* %p
      %b = load i8, i8* %p
      %ok = icmp eq i8 %b, %v
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  auto p = IcfgToNwa::build(f);
  TransitionFormulaBuilder builder(p);
  for (unsigned j = 0; j < builder.inputs().size(); ++j)
    EXPECT_FALSE(hasBitVector(builder.inputs()[j].get_sort()));
  for (unsigned letter = 0; letter < p.statements.size(); ++letter) {
    std::vector<z3::expr> todo{builder.build(letter).relation};
    std::set<unsigned> visited;
    while (!todo.empty()) {
      auto e = todo.back();
      todo.pop_back();
      if (!visited.insert(e.id()).second)
        continue;
      EXPECT_FALSE(hasBitVector(e.get_sort()));
      if (e.is_quantifier())
        todo.push_back(e.body());
      if (e.is_app())
        for (unsigned j = 0; j < e.num_args(); ++j)
          todo.push_back(e.arg(j));
    }
  }
}
