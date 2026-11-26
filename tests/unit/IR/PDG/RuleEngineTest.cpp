#include "IR/PDG/Rules/RuleEngine.h"

#include "IR/PDG/Analyses/ArithmeticAnalysis.h"
#include "IR/PDG/Analyses/BoundsAnalysis.h"
#include "IR/PDG/Models/FunctionFacts.h"
#include "IR/PDG/Models/ValueFacts.h"
#include "IR/PDG/Analyses/LifetimeAnalysis.h"
#include "IR/PDG/Analyses/StateAnalysis.h"
#include "IR/PDG/Analyses/TaintAnalysis.h"
#include "IR/PDG/Construction/GraphBuilder.h"
#include "IR/PDG/Models/LibraryModels.h"
#include "IR/PDG/Cypher/Cypher.h"
#include "TestUtils/LLVMHelpers.h"

#include <chrono>

#include <gtest/gtest.h>

using namespace llvm;
using namespace pdg;

namespace {
class PDGRuleEngineTest : public ::testing::Test {
protected:
  void SetUp() override { graph.reset(); }
  void TearDown() override { graph.reset(); }

  void load(const char *ir) {
    module = lotus::unittest::parseModuleChecked(context, ir, "RuleEngineTest");
    ASSERT_NE(module, nullptr);
    pdg::GraphBuilder(graph).build(*module);
  }

  RuleEngineResult run(const std::string &id) {
    return RuleEngine(graph).analyze({id}, {}, {}, module.get());
  }

  LLVMContext context;
  std::unique_ptr<Module> module;
  ProgramGraph &graph = ProgramGraph::getInstance();
};

TEST_F(PDGRuleEngineTest, CallsRespectNamespaceSignatureAndIndirectBoundaries) {
  load(R"(
    declare i8* @gets(i8*)
    declare i8* @_ZSt4getsPc(i8*)
    declare i8* @_Z4getsPc(i8*)
    declare i8* @_ZN4user4getsEPc(i8*)
    declare void @gmtime(i8*)
    declare void @gmtime_r(i8*)
    declare void @setjmp(i8*)
    declare void @_ZN4user6setjmpEPc(i8*)
    declare void @setjmp_like(i8*)
    define void @calls(i8* %p, i8* (i8*)* %indirect) {
      %a = call i8* @gets(i8* %p)
      %b = call i8* @_ZSt4getsPc(i8* %p)
      %global_cpp = call i8* @_Z4getsPc(i8* %p)
      %c = call i8* @_ZN4user4getsEPc(i8* %p)
      %d = call i8* %indirect(i8* %p)
      call void @gmtime(i8* %p)
      call void @gmtime_r(i8* %p)
      call void @setjmp(i8* %p)
      call void @_ZN4user6setjmpEPc(i8* %p)
      call void @setjmp_like(i8* %p)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/dangerous-function-overflow").findings.size(), 3u);
  EXPECT_EQ(run("cpp/potentially-dangerous-function").findings.size(), 1u);
  EXPECT_EQ(run("cpp/power-of-10/use-of-jmp").findings.size(), 2u);
}

TEST_F(PDGRuleEngineTest, GetsChecksDeclaredParametersAfterPointerCast) {
  load(R"(
    declare i8* @gets(i8*, i32)
    define void @calls(i8* %p) {
      %x = call i8* bitcast (i8* (i8*, i32)* @gets to i8* (i8*)*)(i8* %p)
      ret void
    }
  )");
  EXPECT_TRUE(run("cpp/dangerous-function-overflow").findings.empty());
}

TEST_F(PDGRuleEngineTest, ResolvesAliasCallsAndExposesSemanticCypherProperties) {
  load(R"(
    target datalayout = "e-p:64:64"
    define i8* @gets(i8* %p) { ret i8* %p }
    @alias = alias i8* (i8*), i8* (i8*)* @gets
    declare i8* @strncpy(i8*, i8*, i64)
    define void @calls(i8* %source) {
      %buf = alloca [8 x i8]
      %p = getelementptr [8 x i8], [8 x i8]* %buf, i32 0, i32 0
      %a = call i8* @alias(i8* %p)
      %b = call i8* @strncpy(i8* %p, i8* %source, i64 9)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/dangerous-function-overflow").findings.size(), 1u);
  CypherParser parser;
  auto query = parser.parse("MATCH (c:INST_FUNCALL) WHERE c.arg_count = 3 "
                            "AND c.arg2_int = 9 AND c.arg0_object_bytes = 8 "
                            "RETURN c");
  ASSERT_NE(query, nullptr);
  auto result = CypherQueryExecutor(graph).execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getCount(), 1u);
  auto *call = cast<CallBase>(result->getNodes().front()->getValue());
  EXPECT_EQ(ValueFacts::argument(*call, 99), nullptr);
}

TEST_F(PDGRuleEngineTest, CopyBoundsHandleWideCharactersUnknownAndZeroObjects) {
  load(R"(
    target datalayout = "e-p:64:64-i32:32"
    declare i8* @strncpy(i8*, i8*, i64)
    declare i32* @wcsncpy(i32*, i32*, i64)
    define void @copies(i8* %unknown, i8* %src, i32* %wide_src) {
      %buf = alloca [8 x i8]
      %p = getelementptr [8 x i8], [8 x i8]* %buf, i32 0, i32 0
      %empty = alloca [0 x i8]
      %e = bitcast [0 x i8]* %empty to i8*
      %a = call i8* @strncpy(i8* %p, i8* %src, i64 8)
      %b = call i8* @strncpy(i8* %p, i8* %src, i64 9)
      %c = call i8* @strncpy(i8* %unknown, i8* %src, i64 100)
      %d = call i8* @strncpy(i8* %e, i8* %src, i64 100)
      %wbuf = alloca [8 x i32]
      %w = getelementptr [8 x i32], [8 x i32]* %wbuf, i32 0, i32 0
      %f = call i32* @wcsncpy(i32* %w, i32* %wide_src, i64 8)
      %g = call i32* @wcsncpy(i32* %w, i32* %wide_src, i64 9)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/bad-strncpy-size").findings.size(), 2u);
}

TEST_F(PDGRuleEngineTest, SourceLengthTracksAffineOffsetsAndSafeIdentities) {
  load(R"(
    declare i8* @strncpy(i8*, i8*, i64)
    declare i64 @strlen(i8*)
    define void @copies(i8* %dst, i8* %src, i8* %other) {
      %len = call i64 @strlen(i8* %src)
      %a = call i8* @strncpy(i8* %dst, i8* %src, i64 %len)
      %b = call i8* @strncpy(i8* %src, i8* %src, i64 %len)
      %c = call i8* @strncpy(i8* %dst, i8* %other, i64 %len)
      %plus = add i64 %len, 1
      %d = call i8* @strncpy(i8* %dst, i8* %src, i64 %plus)
      %minus = sub i64 %len, 1
      %e = call i8* @strncpy(i8* %dst, i8* %src, i64 %minus)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/bad-strncpy-size").findings.size(), 2u);
}

TEST_F(PDGRuleEngineTest, StackReturnRequiresEveryOriginToBeLocal) {
  load(R"(
    @global = global i32 0
    define i32* @direct() {
      %local = alloca i32
      ret i32* %local
    }
    define i32* @mixed(i1 %c, i32* %argument) {
      %local = alloca i32
      %p = select i1 %c, i32* %local, i32* %argument
      ret i32* %p
    }
    define i32* @both(i1 %c) {
      %a = alloca i32
      %b = alloca i32
      %p = select i1 %c, i32* %a, i32* %b
      ret i32* %p
    }
    define i32* @static_object() { ret i32* @global }
    define i32* @identity(i32* %argument) { ret i32* %argument }
    define i32* @stack_pointer() {
      %a = alloca i32
      ret i32* %a
    }
    define i32 @scalar() {
      %a = alloca i32
      %v = load i32, i32* %a
      ret i32 %v
    }
  )");
  auto result = run("cpp/return-stack-allocated-memory");
  ASSERT_EQ(result.findings.size(), 2u);
  EXPECT_EQ(result.findings[0].evidence.size(), 1u);
  EXPECT_EQ(result.findings[1].evidence.size(), 2u);
}

TEST_F(PDGRuleEngineTest, UnknownRulesFailAndCriteriaIntersectScope) {
  load(R"(
    declare i8* @gets(i8*)
    define void @first(i8* %p) { %a = call i8* @gets(i8* %p) ret void }
    define void @second(i8* %p) { %a = call i8* @gets(i8* %p) ret void }
  )");
  EXPECT_THROW(run("cpp/not-implemented"), std::invalid_argument);
  PDGCriteria criteria;
  criteria.callee_names.push_back("gets");
  PDGQueryOptions options;
  options.scope = PDGQueryScope::functionScope(*module->getFunction("second"));
  auto result = RuleEngine(graph).analyze({"cpp/dangerous-function-overflow"},
                                         criteria, options, module.get());
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_EQ(result.findings[0].site->getFunc()->getName(), "second");
  EXPECT_FALSE(result.rules[0].coverage.empty());
}

TEST_F(PDGRuleEngineTest,
       LoadEquivalenceRejectsClobbersButRecognizesRepeatedFields) {
  load(R"(
    declare i64 @strlen(i8*)
    declare i8* @strncpy(i8*, i8*, i64)
    declare void @unknown(i8**)
    define void @copies(i8* %dst, i8** %field) {
      %a = load i8*, i8** %field
      %length = call i64 @strlen(i8* %a)
      %b = load i8*, i8** %field
      %bad = call i8* @strncpy(i8* %dst, i8* %b, i64 %length)
      call void @unknown(i8** %field)
      %changed = load i8*, i8** %field
      %unproved = call i8* @strncpy(i8* %dst, i8* %changed, i64 %length)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/bad-strncpy-size").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest,
       FormatParsingHandlesPositionsStarsSuppressionAndUnknownSyntax) {
  auto positional = parseFormat("%2$*1$d %% %3$s", false);
  ASSERT_TRUE(positional);
  EXPECT_EQ(positional->required_arguments, 3u);
  auto stars = parseFormat("%*.*f", false);
  ASSERT_TRUE(stars);
  EXPECT_EQ(stars->required_arguments, 3u);
  EXPECT_FALSE(parseFormat("%2$d %s", false));
  EXPECT_FALSE(parseFormat("%*.*Y", false));
  EXPECT_FALSE(parseFormat("%", false));
  auto scan = parseFormat("%%s %*s %9s %ls %[a-z]", true);
  ASSERT_TRUE(scan);
  EXPECT_EQ(scan->required_arguments, 3u);
  EXPECT_TRUE(scan->unbounded_string);
  EXPECT_FALSE(parseFormat("%9s %*s %%s %ms", true)->unbounded_string);
  load(R"(
    @format = private constant [6 x i8] c"%d %d\00"
    declare i32 @printf(i8*, ...)
    define void @calls() {
      %p = getelementptr [6 x i8], [6 x i8]* @format, i32 0, i32 0
      %a = call i32 (i8*, ...) @printf(i8* %p, i32 1)
      %b = call i32 (i8*, ...) @printf(i8* %p, i32 1, i32 2)
      %c = call i32 (i8*, ...) @printf(i8* %p, i32 1, i32 2, i32 3)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/wrong-number-format-arguments").findings.size(), 1u);
  EXPECT_EQ(run("cpp/too-many-format-arguments").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, RecursionDetectsMutualCyclesAndExcludesOneWayCalls) {
  load(R"(
    define void @a() { call void @b() ret void }
    define void @b() { call void @a() ret void }
    define void @c() { call void @a() ret void }
    define void @self() { call void @self() ret void }
  )");
  EXPECT_EQ(run("cpp/power-of-10/use-of-recursion").findings.size(), 3u);
  EXPECT_EQ(run("cpp/jpl-c/recursion").findings.size(), 3u);
}

TEST_F(PDGRuleEngineTest,
       AllocationFamiliesDistinguishArraysHeapAndUnknownOrigins) {
  load(R"(
    declare i8* @_Znwm(i64)
    declare i8* @_Znam(i64)
    declare void @_ZdlPv(i8*)
    declare void @_ZdaPv(i8*)
    declare i8* @malloc(i64)
    declare void @free(i8*)
    define void @lifetimes(i8* %unknown) {
      %single = call i8* @_Znwm(i64 8)
      %array = call i8* @_Znam(i64 8)
      %heap = call i8* @malloc(i64 8)
      call void @free(i8* %single)
      call void @_ZdlPv(i8* %heap)
      call void @_ZdlPv(i8* %array)
      call void @_ZdaPv(i8* %single)
      call void @free(i8* %heap)
      call void @free(i8* %unknown)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/new-free-mismatch").findings.size(), 2u);
  EXPECT_EQ(run("cpp/new-array-delete-mismatch").findings.size(), 1u);
  EXPECT_EQ(run("cpp/new-delete-array-mismatch").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, ArithmeticUsesSignednessRangesAndTargetPointerWidth) {
  load(R"(
    target datalayout = "e-p:64:64"
    define i1 @math(i32 %x, i16 %small, i8* %p, double %a, double %b) {
      %r = srem i32 %x, 2
      %odd = icmp eq i32 %r, 1
      %nonnegative = zext i16 %small to i32
      %goodrem = srem i32 %nonnegative, 2
      %goododd = icmp eq i32 %goodrem, 1
      %sum = add nsw i32 %x, 8
      %overflow = icmp slt i32 %sum, %x
      %smallsum = add nsw i32 %nonnegative, 8
      %bounded = icmp slt i32 %smallsum, %nonnegative
      %narrow = ptrtoint i8* %p to i32
      %full = ptrtoint i8* %p to i64
      %flags = ptrtoint i8* %p to i16
      %masked = and i16 %flags, 3
      %different = fcmp oeq double %a, %b
      %self = fcmp oeq double %a, %a
      %literal = fcmp oeq double %a, 0.0
      ret i1 %odd
    }
  )");
  EXPECT_EQ(run("cpp/incomplete-parity-check").findings.size(), 1u);
  EXPECT_EQ(run("cpp/signed-overflow-check").findings.size(), 1u);
  EXPECT_EQ(run("cpp/lossy-pointer-cast").findings.size(), 1u);
  EXPECT_EQ(run("cpp/equality-on-floats").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, FileFlagsComeFromDebugMacrosAndModesRespectUmask) {
  load(R"(
    declare i32 @open(i8*, i32, ...)
    declare i32 @umask(i32)
    declare i32 @creat(i8*, i32)
    declare i32 @chmod(i8*, i32)
    define void @files(i8* %path) {
      %missing = call i32 (i8*, i32, ...) @open(i8* %path, i32 512)
      %provided = call i32 (i8*, i32, ...) @open(i8* %path, i32 512, i32 384)
      %otherflag = call i32 (i8*, i32, ...) @open(i8* %path, i32 64)
      %a = call i32 @creat(i8* %path, i32 438)
      %mask = call i32 @umask(i32 2)
      %b = call i32 @creat(i8* %path, i32 438)
      %c = call i32 @chmod(i8* %path, i32 438)
      ret void
    }
    !llvm.dbg.cu = !{!0}
    !llvm.module.flags = !{!4}
    !0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "test", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, macros: !2)
    !1 = !DIFile(filename: "file.c", directory: "/tmp")
    !2 = !{!3}
    !3 = !DIMacro(type: DW_MACINFO_define, line: 1, name: "O_CREAT", value: "0x0200")
    !4 = !{i32 2, !"Debug Info Version", i32 3}
  )");
  EXPECT_EQ(run("cpp/open-call-with-mode-argument").findings.size(), 1u);
  EXPECT_EQ(run("cpp/world-writable-file-creation").findings.size(), 3u);
  EXPECT_FALSE(ValueFacts::macroInteger(*module, "O_TMPFILE"));
}

TEST_F(PDGRuleEngineTest, ScanfCurlAndTemporaryNamesUseApiRolesAndCfg) {
  load(R"(
    @unsafe = private constant [3 x i8] c"%s\00"
    @bounded = private constant [4 x i8] c"%9s\00"
    declare i32 @scanf(i8*, ...)
    declare i32 @curl_easy_setopt(i8*, i32, ...)
    declare i8* @tmpnam(i8*)
    declare i32 @mkstemp(i8*)
    define void @calls(i8* %p) {
      %fmt = getelementptr [3 x i8], [3 x i8]* @unsafe, i32 0, i32 0
      %limited = getelementptr [4 x i8], [4 x i8]* @bounded, i32 0, i32 0
      %a = call i32 (i8*, ...) @scanf(i8* %fmt, i8* %p)
      %b = call i32 (i8*, ...) @scanf(i8* %limited, i8* %p)
      %zero = icmp ne i32 %a, 0
      br i1 %zero, label %yes, label %no
    yes:
      %bad = call i32 (i8*, i32, ...) @curl_easy_setopt(i8* %p, i32 64, i64 0)
      %good = call i32 (i8*, i32, ...) @curl_easy_setopt(i8* %p, i32 81, i64 2)
      ret void
    no:
      ret void
    }
    define void @insecure(i8* %p) { %a = call i8* @tmpnam(i8* %p) ret void }
    define void @suppressed(i8* %p) {
      %a = call i8* @tmpnam(i8* %p)
      %b = call i32 @mkstemp(i8* %p)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/memory-unsafe-function-scan").findings.size(), 1u);
  EXPECT_EQ(run("cpp/incorrectly-checked-scanf").findings.size(), 1u);
  EXPECT_EQ(run("cpp/curl-disabled-ssl").findings.size(), 1u);
  EXPECT_EQ(run("cpp/insecure-generation-of-filename").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest,
       LoopBoundsDistinguishSmallInfiniteAndOneShotBooleanLoops) {
  load(R"(
    define void @small() {
    entry:
      br label %head
    head:
      %i = phi i32 [ 0, %entry ], [ %next, %body ]
      %condition = icmp slt i32 %i, 2
      br i1 %condition, label %body, label %exit
    body:
      %allocation = alloca i8, i64 16
      %next = add nsw i32 %i, 1
      br label %head
    exit:
      ret void
    }
    define void @forever() {
    entry:
      br label %body
    body:
      %allocation = alloca i8, i64 16
      br label %body
    }
    define void @once() {
    entry:
      br label %head
    head:
      %stop = phi i1 [ false, %entry ], [ true, %body ]
      br i1 %stop, label %exit, label %body
    body:
      %allocation = alloca i8, i64 16
      br label %head
    exit:
      ret void
    }
  )");
  auto result = run("cpp/alloca-in-loop");
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_EQ(result.findings[0].site->getFunc()->getName(), "forever");
}

TEST_F(PDGRuleEngineTest,
       MandatoryReturnChecksAndRetainedDeadFunctionsHavePositiveCases) {
  load(R"(
    declare i8* @fgets(i8*, i32, i8*)
    define void @reader(i8* %p, i8* %file) {
      %ignored = call i8* @fgets(i8* %p, i32 10, i8* %file)
      %checked = call i8* @fgets(i8* %p, i32 10, i8* %file)
      %valid = icmp ne i8* %checked, null
      br i1 %valid, label %yes, label %no
    yes:
      ret void
    no:
      ret void
    }
    define internal void @unused() { ret void }
    define internal void @used() { ret void }
    define void @caller() { call void @used() ret void }
  )");
  EXPECT_EQ(run("cpp/return-value-ignored").findings.size(), 1u);
  EXPECT_EQ(run("cpp/dead-code-function").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, StrncatReserveByteIsNotConfusedWithLengthProvenance) {
  load(R"(
    target datalayout = "e-p:64:64"
    declare i64 @strlen(i8*)
    declare i8* @strncat(i8*, i8*, i64)
    define void @append(i8* %source) {
      %buffer = alloca [8 x i8]
      %destination = getelementptr [8 x i8], [8 x i8]* %buffer, i32 0, i32 0
      %length = call i64 @strlen(i8* %destination)
      %remaining = sub i64 8, %length
      %bad = call i8* @strncat(i8* %destination, i8* %source, i64 %remaining)
      %reserved = sub i64 %remaining, 1
      %good = call i8* @strncat(i8* %destination, i8* %source, i64 %reserved)
      %length_with_null = add i64 %length, 1
      %available = sub i64 8, %length_with_null
      %also_good = call i8* @strncat(i8* %destination, i8* %source, i64 %available)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/unsafe-strncat").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest,
       CypherSemanticPredicatesCompareFactsAndPreserveUnknowns) {
  load(R"(
    target datalayout = "e-p:64:64-i32:32"
    @format = private constant [6 x i8] c"%d %d\00"
    declare i8* @strncpy(i8*, i8*, i64)
    declare i32* @wcsncpy(i32*, i32*, i64)
    declare i32 @printf(i8*, ...)
    define void @calls(i8* %source, i8* %unknown, i32* %wide_source) {
      %buffer = alloca [8 x i8]
      %p = getelementptr [8 x i8], [8 x i8]* %buffer, i32 0, i32 0
      %safe = call i8* @strncpy(i8* %p, i8* %source, i64 8)
      %bad = call i8* @strncpy(i8* %p, i8* %source, i64 9)
      %not_known = call i8* @strncpy(i8* %unknown, i8* %source, i64 9)
      %wide_buffer = alloca [8 x i32]
      %w = getelementptr [8 x i32], [8 x i32]* %wide_buffer, i32 0, i32 0
      %wide_bad = call i32* @wcsncpy(i32* %w, i32* %wide_source, i64 4600000000000000000)
      %fmt = getelementptr [6 x i8], [6 x i8]* @format, i32 0, i32 0
      %missing = call i32 (i8*, ...) @printf(i8* %fmt, i32 1)
      ret void
    }
  )");
  CypherParser parser;
  CypherQueryExecutor executor(graph);
  auto query = parser.parse("MATCH (c:INST_FUNCALL) WHERE c.copy_size_bytes > "
                            "c.copy_destination_bytes RETURN c");
  ASSERT_NE(query, nullptr);
  auto result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getCount(), 2u);
  query = parser.parse("MATCH (c:INST_FUNCALL) WHERE c.copy_size_bytes != "
                       "c.copy_destination_bytes RETURN c");
  ASSERT_NE(query, nullptr);
  result = executor.execute(*query);
  EXPECT_EQ(result->getCount(), 2u);
  query = parser.parse("MATCH (c:INST_FUNCALL) WHERE c.format_given < "
                       "c.format_expected RETURN c");
  ASSERT_NE(query, nullptr);
  result = executor.execute(*query);
  EXPECT_EQ(result->getCount(), 1u);
  query = parser.parse("MATCH (c:INST_FUNCALL) WHERE c.func = "
                       "'c.copy_destination_bytes' RETURN c");
  ASSERT_NE(query, nullptr);
  EXPECT_EQ(executor.execute(*query)->getCount(), 0u);
}

TEST_F(PDGRuleEngineTest,
       TaintRulesDistinguishWholeCommandsFromDangerousConcatenation) {
  load(R"(
    @key = private constant [2 x i8] c"X\00"
    @whole = private constant [3 x i8] c"%s\00"
    @prefix = private constant [5 x i8] c"x %s\00"
    declare i8* @getenv(i8*)
    declare i32 @system(i8*)
    declare i32 @sprintf(i8*, i8*, ...)
    declare i32 @printf(i8*, ...)
    declare i8* @fopen(i8*, i8*)
    declare i32 @sqlite3_exec(i8*, i8*, i8*, i8*, i8*)
    define void @entry() {
      %key = getelementptr [2 x i8], [2 x i8]* @key, i32 0, i32 0
      %user = call i8* @getenv(i8* %key)
      %a = call i32 @system(i8* %user)
      %f1 = getelementptr [3 x i8], [3 x i8]* @whole, i32 0, i32 0
      %f2 = getelementptr [5 x i8], [5 x i8]* @prefix, i32 0, i32 0
      %b1 = alloca i8, i64 128
      %b2 = alloca i8, i64 128
      %w1 = call i32 (i8*, i8*, ...) @sprintf(i8* %b1, i8* %f1, i8* %user)
      %w2 = call i32 (i8*, i8*, ...) @sprintf(i8* %b2, i8* %f2, i8* %user)
      %b = call i32 @system(i8* %b1)
      %c = call i32 @system(i8* %b2)
      %d = call i32 (i8*, ...) @printf(i8* %user)
      %e = call i8* @fopen(i8* %user, i8* %key)
      %sql = call i32 @sqlite3_exec(i8* null, i8* %user, i8* null, i8* null, i8* null)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/uncontrolled-process-operation").findings.size(), 3u);
  auto command = run("cpp/command-line-injection");
  ASSERT_EQ(command.findings.size(), 1u);
  ASSERT_FALSE(command.findings[0].taint_origins.empty());
  EXPECT_NE(command.findings[0].taint_origins[0].concatenation, nullptr);
  EXPECT_EQ(run("cpp/tainted-format-string").findings.size(), 1u);
  EXPECT_EQ(run("cpp/non-constant-format").findings.size(), 1u);
  EXPECT_EQ(run("cpp/path-injection").findings.size(), 1u);
  EXPECT_EQ(run("cpp/sql-injection").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, TaintStrongUpdatesRespectCleanCopiesAndCalleeWrites) {
  load(R"(
    @literal = private constant [5 x i8] c"safe\00"
    declare i8* @getenv(i8*)
    declare i8* @strcpy(i8*, i8*)
    declare i32 @system(i8*)
    define void @clear(i8* %buffer) {
      %literal = getelementptr [5 x i8], [5 x i8]* @literal, i32 0, i32 0
      %a = call i8* @strcpy(i8* %buffer, i8* %literal)
      ret void
    }
    define void @entry() {
      %literal = getelementptr [5 x i8], [5 x i8]* @literal, i32 0, i32 0
      %user = call i8* @getenv(i8* %literal)
      %buffer = alloca i8, i64 16
      %taint1 = call i8* @strcpy(i8* %buffer, i8* %user)
      %bad = call i32 @system(i8* %buffer)
      %reset = call i8* @strcpy(i8* %buffer, i8* %literal)
      %good1 = call i32 @system(i8* %buffer)
      %taint2 = call i8* @strcpy(i8* %buffer, i8* %user)
      call void @clear(i8* %buffer)
      %good2 = call i32 @system(i8* %buffer)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/uncontrolled-process-operation").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest,
       TaintCallReturnMatchingDoesNotMixCleanAndTaintedInvocations) {
  load(R"(
    @literal = private constant [5 x i8] c"safe\00"
    declare i8* @getenv(i8*)
    declare i32 @system(i8*)
    define i8* @identity(i8* %p) { ret i8* %p }
    define void @entry() {
      %literal = getelementptr [5 x i8], [5 x i8]* @literal, i32 0, i32 0
      %user = call i8* @getenv(i8* %literal)
      %tainted = call i8* @identity(i8* %user)
      %clean = call i8* @identity(i8* %literal)
      %bad = call i32 @system(i8* %tainted)
      %good = call i32 @system(i8* %clean)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/uncontrolled-process-operation").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, TaintFlowPreservesFixedFieldSeparationAndCfgOrder) {
  load(R"(
    target datalayout = "e-p:64:64"
    %Buffers = type { [8 x i8], [8 x i8] }
    declare i8* @fgets(i8*, i32, i8*)
    declare i32 @system(i8*)
    define void @fields(i8* %file) {
      %object = alloca %Buffers
      %a = getelementptr %Buffers, %Buffers* %object, i32 0, i32 0, i32 0
      %b = getelementptr %Buffers, %Buffers* %object, i32 0, i32 1, i32 0
      %input = call i8* @fgets(i8* %a, i32 8, i8* %file)
      %bad = call i32 @system(i8* %a)
      %good = call i32 @system(i8* %b)
      ret void
    }
    define void @exclusive(i1 %condition, i8* %file) {
    entry:
      %buffer = alloca i8, i64 8
      br i1 %condition, label %read, label %sink
    read:
      %input = call i8* @fgets(i8* %buffer, i32 8, i8* %file)
      br label %exit
    sink:
      %good = call i32 @system(i8* %buffer)
      br label %exit
    exit:
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/uncontrolled-process-operation").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, SqlEscapingIsNotAGlobalSanitizerForOtherDomains) {
  load(R"(
    @key = private constant [2 x i8] c"X\00"
    declare i8* @getenv(i8*)
    declare i64 @mysql_real_escape_string(i8*, i8*, i8*, i64)
    declare i32 @mysql_query(i8*, i8*)
    declare i32 @system(i8*)
    define void @entry(i8* %connection) {
      %key = getelementptr [2 x i8], [2 x i8]* @key, i32 0, i32 0
      %user = call i8* @getenv(i8* %key)
      %buffer = alloca i8, i64 128
      %escaped = call i64 @mysql_real_escape_string(i8* %connection, i8* %buffer, i8* %user, i64 16)
      %good = call i32 @mysql_query(i8* %connection, i8* %buffer)
      %bad = call i32 @system(i8* %buffer)
      ret void
    }
  )");
  EXPECT_TRUE(run("cpp/sql-injection").findings.empty());
  EXPECT_EQ(run("cpp/uncontrolled-process-operation").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest,
       NumericFormattingAndScanfLiteralInputDoNotIntroduceStringTaint) {
  load(R"(
    @key = private constant [2 x i8] c"X\00"
    @numeric = private constant [3 x i8] c"%d\00"
    @literal = private constant [5 x i8] c"safe\00"
    @string = private constant [3 x i8] c"%s\00"
    declare i8* @getenv(i8*)
    declare i32 @atoi(i8*)
    declare i32 @sprintf(i8*, i8*, ...)
    declare i32 @sscanf(i8*, i8*, ...)
    declare i32 @system(i8*)
    define void @entry() {
      %key = getelementptr [2 x i8], [2 x i8]* @key, i32 0, i32 0
      %fmt = getelementptr [3 x i8], [3 x i8]* @numeric, i32 0, i32 0
      %text = getelementptr [5 x i8], [5 x i8]* @literal, i32 0, i32 0
      %sfmt = getelementptr [3 x i8], [3 x i8]* @string, i32 0, i32 0
      %user = call i8* @getenv(i8* %key)
      %number = call i32 @atoi(i8* %user)
      %buffer = alloca i8, i64 16
      %a = call i32 (i8*, i8*, ...) @sprintf(i8* %buffer, i8* %fmt, i32 %number)
      %good1 = call i32 @system(i8* %buffer)
      %b = call i32 (i8*, i8*, ...) @sscanf(i8* %text, i8* %sfmt, i8* %buffer)
      %good2 = call i32 @system(i8* %buffer)
      ret void
    }
  )");
  EXPECT_TRUE(run("cpp/uncontrolled-process-operation").findings.empty());
}

TEST_F(PDGRuleEngineTest,
       NonconstantSourcesRemainSeparateFromUserInputAndUnknownAllocations) {
  load(R"(
    declare i32 @printf(i8*, ...)
    declare i8* @malloc(i64)
    define void @exported(i8* %format) {
      %recommendation = call i32 (i8*, ...) @printf(i8* %format)
      %uninitialized = call i8* @malloc(i64 16)
      %other_checker = call i32 (i8*, ...) @printf(i8* %uninitialized)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/non-constant-format").findings.size(), 1u);
  EXPECT_TRUE(run("cpp/tainted-format-string").findings.empty());
}

TEST_F(PDGRuleEngineTest,
       TransparentSinkWrappersAreSummarizedAndReportedAtTheCaller) {
  load(R"(
    @key = private constant [2 x i8] c"X\00"
    declare i8* @getenv(i8*)
    declare i32 @sqlite3_exec(i8*, i8*, i8*, i8*, i8*)
    define void @wrapper(i8* %query) {
      %a = call i32 @sqlite3_exec(i8* null, i8* %query, i8* null, i8* null, i8* null)
      ret void
    }
    define void @entry() {
      %key = getelementptr [2 x i8], [2 x i8]* @key, i32 0, i32 0
      %user = call i8* @getenv(i8* %key)
      call void @wrapper(i8* %user)
      call void @wrapper(i8* %key)
      ret void
    }
  )");
  auto result = run("cpp/sql-injection");
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_EQ(result.findings[0].site->getFunc()->getName(), "entry");
}

TEST_F(PDGRuleEngineTest, MainArgvIsAContentSourceAndLimitsAreReported) {
  load(R"(
    declare i32 @system(i8*)
    define i32 @main(i32 %argc, i8** %argv) {
      %slot = getelementptr i8*, i8** %argv, i32 1
      %arg = load i8*, i8** %slot
      %bad = call i32 @system(i8* %arg)
      ret i32 0
    }
  )");
  EXPECT_EQ(run("cpp/uncontrolled-process-operation").findings.size(), 1u);
  TaintPolicy policy;
  policy.max_steps = 1;
  EXPECT_TRUE(
      TaintAnalysis(graph).analyze(*module, policy).diagnostics.state_limit_hit);
}

TEST_F(PDGRuleEngineTest, NativeTaintSummariesConvergeForRecursivePointerFlow) {
  load(R"(
    @safe = private constant [5 x i8] c"safe\00"
    declare i8* @getenv(i8*)
    declare i32 @system(i8*)
    define i8* @recursive(i8* %p, i1 %done) {
    entry:
      br i1 %done, label %base, label %again
    base:
      ret i8* %p
    again:
      %next = getelementptr i8, i8* %p, i32 1
      %result = call i8* @recursive(i8* %next, i1 true)
      ret i8* %result
    }
    define void @entry() {
      %safe = getelementptr [5 x i8], [5 x i8]* @safe, i32 0, i32 0
      %user = call i8* @getenv(i8* %safe)
      %tainted = call i8* @recursive(i8* %user, i1 false)
      %clean = call i8* @recursive(i8* %safe, i1 false)
      %bad = call i32 @system(i8* %tainted)
      %good = call i32 @system(i8* %clean)
      ret void
    }
  )");
  auto result = run("cpp/uncontrolled-process-operation");
  EXPECT_EQ(result.findings.size(), 1u);
  EXPECT_FALSE(result.diagnostics.state_limit_hit);
  EXPECT_GT(result.diagnostics.summary_cache_hits, 0u);
}

TEST_F(PDGRuleEngineTest, AllocationSizesUseScalarTaintAndDominatingBounds) {
  load(R"(
    @key = private constant [2 x i8] c"X\00"
    declare i8* @getenv(i8*)
    declare i64 @strtoul(i8*, i8**, i32)
    declare i8* @malloc(i64)
    define void @entry() {
      %key = getelementptr [2 x i8], [2 x i8]* @key, i32 0, i32 0
      %user = call i8* @getenv(i8* %key)
      %n = call i64 @strtoul(i8* %user, i8** null, i32 10)
      %bad = call i8* @malloc(i64 %n)
      %masked = and i64 %n, 255
      %safe_mask = call i8* @malloc(i64 %masked)
      %bounded = icmp ult i64 %n, 1024
      br i1 %bounded, label %inside, label %exit
    inside:
      %safe_guard = call i8* @malloc(i64 %n)
      %scaled = mul i64 %n, 16
      %safe_scale = call i8* @malloc(i64 %scaled)
      br label %exit
    exit:
      ret void
    }
  )");
  auto result = run("cpp/uncontrolled-allocation-size");
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_FALSE(result.findings[0].taint_origins.empty());
}

TEST_F(PDGRuleEngineTest,
       SignedUpperBoundsAndLateLoopTestsAreNotSafeAllocationGuards) {
  load(R"(
    @key = private constant [2 x i8] c"X\00"
    declare i8* @getenv(i8*)
    declare i64 @strtol(i8*, i8**, i32)
    declare i8* @malloc(i64)
    define void @entry() {
      %key = getelementptr [2 x i8], [2 x i8]* @key, i32 0, i32 0
      %user = call i8* @getenv(i8* %key)
      %n = call i64 @strtol(i8* %user, i8** null, i32 10)
      %upper = icmp slt i64 %n, 1024
      br i1 %upper, label %possibly_negative, label %loop
    possibly_negative:
      %bad_negative = call i8* @malloc(i64 %n)
      br label %loop
    loop:
      %bad_late = call i8* @malloc(i64 %n)
      %test = icmp ult i64 %n, 128
      br i1 %test, label %loop, label %exit
    exit:
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/uncontrolled-allocation-size").findings.size(), 2u);
}

TEST_F(PDGRuleEngineTest, CrossBlockLoadGuardsRequireTheSameMemoryVersion) {
  load(R"(
    define i8* @checked(i64* %slot) {
    entry:
      %before = load i64, i64* %slot
      %condition = icmp ult i64 %before, 128
      br i1 %condition, label %inside, label %exit
    inside:
      %after = load i64, i64* %slot
      %first = call i8* @malloc(i64 %after)
      store i64 1000, i64* %slot
      %changed = load i64, i64* %slot
      %second = call i8* @malloc(i64 %changed)
      ret i8* %second
    exit:
      ret i8* null
    }
    declare i8* @malloc(i64)
  )");
  Function &function = *module->getFunction("checked");
  FunctionFacts facts(function);
  auto *before = &*function.getEntryBlock().begin();
  auto *inside = function.getEntryBlock().getTerminator()->getSuccessor(0);
  auto it = inside->begin();
  Instruction *after = &*it++;
  Instruction *first = &*it++;
  ++it;
  Instruction *changed = &*it++;
  Instruction *second = &*it;
  EXPECT_TRUE(facts.equivalent(*before, *after));
  EXPECT_FALSE(facts.equivalent(*before, *changed));
  EXPECT_TRUE(facts.boundedAt(*after, *first));
  EXPECT_FALSE(facts.boundedAt(*changed, *second));
}

TEST_F(PDGRuleEngineTest,
       NumericReturnSummariesRecognizeGenericClampsWithoutNames) {
  load(R"(
    @key = private constant [2 x i8] c"X\00"
    declare i8* @getenv(i8*)
    declare i64 @strtoul(i8*, i8**, i32)
    declare i8* @malloc(i64)
    define i64 @limit_value(i64 %x, i64 %limit) {
    entry:
      %condition = icmp ult i64 %x, %limit
      br i1 %condition, label %small, label %large
    small:
      br label %exit
    large:
      br label %exit
    exit:
      %result = phi i64 [ %x, %small ], [ %limit, %large ]
      ret i64 %result
    }
    define void @entry() {
      %key = getelementptr [2 x i8], [2 x i8]* @key, i32 0, i32 0
      %user = call i8* @getenv(i8* %key)
      %n = call i64 @strtoul(i8* %user, i8** null, i32 10)
      %limited = call i64 @limit_value(i64 %n, i64 128)
      %scaled = mul i64 %limited, 4
      %good = call i8* @malloc(i64 %scaled)
      %bad = call i8* @malloc(i64 %n)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/uncontrolled-allocation-size").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, SignedClampSummariesRespectConditionalPhiEdges) {
  load(R"(
    define i32 @clamp(i32 %x, i32 %limit) {
    entry:
      %lower = icmp sle i32 %x, 0
      br i1 %lower, label %negative, label %positive
    negative:
      br label %exit
    positive:
      %upper = icmp sgt i32 %x, %limit
      br i1 %upper, label %large, label %merge
    large:
      br label %merge
    merge:
      %r = phi i32 [ %limit, %large ], [ %x, %positive ]
      br label %exit
    exit:
      %result = phi i32 [ 1, %negative ], [ %r, %merge ]
      ret i32 %result
    }
    declare i8* @malloc(i64)
    define i8* @use(i32 %n) {
      %limited = call i32 @clamp(i32 %n, i32 500)
      %wide = sext i32 %limited to i64
      %scaled = mul i64 %wide, 4
      %memory = call i8* @malloc(i64 %scaled)
      ret i8* %memory
    }
  )");
  Function &function = *module->getFunction("use");
  FunctionFacts facts(function);
  auto it = function.getEntryBlock().begin();
  Instruction *limited = &*it++;
  ++it;
  Instruction *scaled = &*it++;
  Instruction *allocation = &*it;
  EXPECT_TRUE(facts.boundedAt(*limited, *allocation));
  EXPECT_TRUE(facts.boundedAt(*scaled, *allocation));
}
TEST_F(PDGRuleEngineTest, WideFormattingModelsResolveCppLinkage) {
  load(R"(
    declare void @_Z16StringCchPrintfWPwjPKwz(i32*, i32, i32*, ...)
    define void @caller(i32* %buffer, i32* %fmt) {
      call void (i32*, i32, i32*, ...) @_Z16StringCchPrintfWPwjPKwz(i32* %buffer, i32 100, i32* %fmt)
      ret void
    }
  )");
  auto *function = module->getFunction("_Z16StringCchPrintfWPwjPKwz");
  EXPECT_EQ(ValueFacts::functionBaseName(*function), "StringCchPrintfW");
  EXPECT_TRUE(
      ValueFacts::hasLibraryName(*function, "StringCchPrintfW", true, true));
  EXPECT_TRUE(LibraryModels::format(*function));
}

TEST_F(PDGRuleEngineTest,
       BoundsServicesCheckSubobjectsWidthsAndZeroLengthOperations) {
  load(R"(
    target datalayout = "e-p:64:64-i32:32"
    %S = type { [4 x i8], [16 x i8] }
    declare i8* @memcpy(i8*, i8*, i64)
    declare i8* @malloc(i64)
    define void @copies(i8* %source) {
      %object = alloca %S
      %a = getelementptr %S, %S* %object, i32 0, i32 0, i32 0
      %b = getelementptr %S, %S* %object, i32 0, i32 1, i32 0
      %bad = call i8* @memcpy(i8* %a, i8* %source, i64 5)
      %good = call i8* @memcpy(i8* %b, i8* %source, i64 16)
      %end = getelementptr i8, i8* %a, i64 4
      %zero = call i8* @memcpy(i8* %end, i8* %source, i64 0)
      %heap = call i8* @malloc(i64 8)
      %typed = bitcast i8* %heap to i32*
      %last = getelementptr i32, i32* %typed, i32 2
      store i32 1, i32* %last
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/overflow-buffer").findings.size(), 2u);
  EXPECT_EQ(run("cpp/static-buffer-overflow").findings.size(), 1u);
  EXPECT_EQ(run("cpp/invalid-pointer-deref").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest,
       BoundsRelationsRecognizeDynamicEndsAndTerminatorSpace) {
  load(R"(
    target datalayout = "e-p:64:64"
    declare i8* @malloc(i64)
    declare i64 @strlen(i8*)
    declare i8* @strcpy(i8*, i8*)
    define void @ends(i64 %n, i8* %source) {
      %heap = call i8* @malloc(i64 %n)
      %end = getelementptr i8, i8* %heap, i64 %n
      store i8 0, i8* %end
      %last_index = sub i64 %n, 1
      %last = getelementptr i8, i8* %heap, i64 %last_index
      store i8 0, i8* %last
      %length = call i64 @strlen(i8* %source)
      %short = call i8* @malloc(i64 %length)
      %copy = call i8* @strcpy(i8* %short, i8* %source)
      %size = add i64 %length, 1
      %enough = call i8* @malloc(i64 %size)
      %good = call i8* @strcpy(i8* %enough, i8* %source)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/invalid-pointer-deref").findings.size(), 1u);
  EXPECT_EQ(run("cpp/no-space-for-terminator").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest,
       LifetimesRespectMutuallyExclusiveReleasePathsAndRefreshes) {
  load(R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)
    define void @exclusive(i8* %p, i1 %condition) {
      br i1 %condition, label %left, label %right
    left:
      call void @free(i8* %p)
      br label %exit
    right:
      call void @free(i8* %p)
      br label %exit
    exit:
      ret void
    }
    define i8 @uaf() {
      %p = call i8* @malloc(i64 8)
      call void @free(i8* %p)
      %v = load i8, i8* %p
      ret i8 %v
    }
    define void @twice() {
      %p = call i8* @malloc(i64 8)
      call void @free(i8* %p)
      call void @free(i8* %p)
      ret void
    }
    define void @refresh() {
      %a = call i8* @malloc(i64 8)
      call void @free(i8* %a)
      %b = call i8* @malloc(i64 8)
      store i8 0, i8* %b
      call void @free(i8* %b)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/double-free").findings.size(), 1u);
  EXPECT_EQ(run("cpp/use-after-free").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, LifetimeLeaksSeparateNeverMayAndEscapingObjects) {
  load(R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare i8* @fopen(i8*, i8*)
    define void @never() {
      %p = call i8* @malloc(i64 8)
      ret void
    }
    define void @sometimes(i1 %condition) {
      %p = call i8* @malloc(i64 8)
      br i1 %condition, label %release, label %exit
    release:
      call void @free(i8* %p)
      br label %exit
    exit:
      ret void
    }
    define i8* @escape() {
      %p = call i8* @malloc(i64 8)
      ret i8* %p
    }
    define void @file_leak(i8* %path) {
      %p = call i8* @fopen(i8* %path, i8* %path)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/memory-never-freed").findings.size(), 1u);
  EXPECT_EQ(run("cpp/memory-may-not-be-freed").findings.size(), 1u);
  EXPECT_EQ(run("cpp/file-never-closed").findings.size(), 1u);
  EXPECT_TRUE(LifetimeAnalysis().analyze(*module, 1).state_limit_hit);
}

TEST_F(PDGRuleEngineTest,
       StateGuardsCheckActualDereferencesAndLaterAssignments) {
  load(R"(
    declare i8* @malloc(i64)
    define i8 @bad() {
      %p = call i8* @malloc(i64 8)
      %v = load i8, i8* %p
      ret i8 %v
    }
    define i8 @good() {
      %p = call i8* @malloc(i64 8)
      %nonnull = icmp ne i8* %p, null
      br i1 %nonnull, label %read, label %exit
    read:
      %v = load i8, i8* %p
      ret i8 %v
    exit:
      ret i8 0
    }
    define i8 @checked_old_value() {
      %old = call i8* @malloc(i64 8)
      %nonnull = icmp ne i8* %old, null
      br i1 %nonnull, label %read, label %exit
    read:
      %new = call i8* @malloc(i64 8)
      %v = load i8, i8* %new
      ret i8 %v
    exit:
      ret i8 0
    }
  )");
  EXPECT_EQ(run("cpp/missing-null-test").findings.size(), 2u);
}

TEST_F(PDGRuleEngineTest, InitializationTracksStrongWritesJoinsAndEscapes) {
  load(R"(
    declare void @writer(i32*)
    define i32 @uninitialized() {
      %x = alloca i32
      %v = load i32, i32* %x
      ret i32 %v
    }
    define i32 @initialized() {
      %x = alloca i32
      store i32 1, i32* %x
      %v = load i32, i32* %x
      ret i32 %v
    }
    define i32 @conditional(i1 %condition) {
      %x = alloca i32
      br i1 %condition, label %write, label %exit
    write:
      store i32 1, i32* %x
      br label %exit
    exit:
      %v = load i32, i32* %x
      ret i32 %v
    }
    define i32 @unknown() {
      %x = alloca i32
      call void @writer(i32* %x)
      %v = load i32, i32* %x
      ret i32 %v
    }
  )");
  EXPECT_EQ(run("cpp/uninitialized-local").findings.size(), 1u);
  EXPECT_EQ(run("cpp/not-initialised").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, CweCatalogRetainsOriginalTagsAndSemanticBoundaries) {
  const auto &catalog = RuleEngine::catalog();
  auto find = [&](const std::string &id) -> const RuleDescriptor & {
    return *std::find_if(catalog.begin(), catalog.end(),
                         [&](const auto &rule) { return rule.id == id; });
  };
  EXPECT_EQ(find("cpp/double-free").cwes, std::vector<unsigned>({415}));
  EXPECT_EQ(find("cpp/use-after-free").cwes, std::vector<unsigned>({416}));
  EXPECT_EQ(find("cpp/uninitialized-local").cwes,
            std::vector<unsigned>({457, 665}));
  EXPECT_FALSE(find("cpp/invalid-pointer-deref").coverage.empty());
}

TEST_F(PDGRuleEngineTest, CypherMemoryFactsRespectWidthsGuardsAndPreStoreState) {
  load(R"(
    target datalayout = "e-p:64:64-i32:32"
    declare i8* @malloc(i64)
    define i32 @facts(i1 %condition) {
      %x = alloca i32
      %uninit = load i32, i32* %x
      store i32 7, i32* %x
      %initialized = load i32, i32* %x
      %b = alloca [4 x i8]
      %end = getelementptr [4 x i8], [4 x i8]* %b, i32 0, i32 4
      store i8 1, i8* %end
      %p = call i8* @malloc(i64 8)
      %unchecked = load i8, i8* %p
      %nonnull = icmp ne i8* %p, null
      br i1 %nonnull, label %read, label %exit
    read:
      %checked = load i8, i8* %p
      br label %exit
    exit:
      ret i32 %initialized
    }
  )");
  CypherParser parser;
  CypherQueryExecutor executor(graph);
  auto count = [&](const char *text) {
    auto query = parser.parse(text);
    EXPECT_NE(query, nullptr);
    if (!query)
      return size_t(0);
    auto result = executor.execute(*query);
    EXPECT_NE(result, nullptr);
    return result ? result->getCount() : size_t(0);
  };
  EXPECT_EQ(count("MATCH (n:INST) WHERE n.write_out_of_bounds = true AND "
                  "n.access_offset_bytes = 4 AND n.access_capacity_bytes = 4 "
                  "AND n.access_bytes = 1 RETURN n"),
            1u);
  EXPECT_EQ(count("MATCH (n:INST) WHERE n.name = 'uninit' AND "
                  "n.pointee_initialization = 'uninitialized' RETURN n"),
            1u);
  EXPECT_EQ(
      count(
          "MATCH (n:INST) WHERE n.opcode = 'store' AND n.access_bytes = 4 AND "
          "n.pointee_initialization = 'uninitialized' RETURN n"),
      1u);
  EXPECT_EQ(count("MATCH (n:INST) WHERE n.name = 'initialized' AND "
                  "n.pointee_initialization = 'initialized' RETURN n"),
            1u);
  EXPECT_EQ(count("MATCH (n:INST) WHERE n.opcode = 'load' AND "
                  "n.pointer_nullness = 'nullable' RETURN n"),
            1u);
  EXPECT_EQ(count("MATCH (n:INST) WHERE n.name = 'checked' AND "
                  "n.pointer_nullness = 'nonnull' RETURN n"),
            1u);
  // A new query observes an in-place IR edit even if the graph epoch is stable.
  auto *function = module->getFunction("facts");
  StoreInst *firstStore = nullptr;
  for (auto &inst : function->getEntryBlock())
    if (auto *store = dyn_cast<StoreInst>(&inst)) {
      firstStore = store;
      break;
    }
  ASSERT_NE(firstStore, nullptr);
  firstStore->setOperand(0, UndefValue::get(Type::getInt32Ty(context)));
  EXPECT_EQ(count("MATCH (n:INST) WHERE n.name = 'initialized' AND "
                  "n.pointee_initialization = 'uninitialized' RETURN n"),
            1u);
}

TEST_F(PDGRuleEngineTest, ResourcePredicatesExcludeComplementaryReleasePaths) {
  load(R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare i32 @open(i8*, i32)
    declare i32 @close(i32)
    declare i8* @fopen(i8*, i8*)
    declare i32 @fclose(i8*)
    define void @complementary(i32 %x) {
      %p = call i8* @malloc(i64 8)
      %positive = icmp sgt i32 %x, 0
      br i1 %positive, label %release1, label %next
    release1:
      call void @free(i8* %p)
      br label %next
    next:
      %nonpositive = icmp sge i32 0, %x
      br i1 %nonpositive, label %release2, label %exit
    release2:
      call void @free(i8* %p)
      br label %exit
    exit:
      ret void
    }
    define void @descriptor_leak(i8* %path) {
      %fd = call i32 @open(i8* %path, i32 0)
      ret void
    }
    define void @descriptor_safe(i8* %path) {
      %fd = call i32 @open(i8* %path, i32 0)
      %valid = icmp sge i32 %fd, 0
      br i1 %valid, label %release, label %exit
    release:
      %ignored = call i32 @close(i32 %fd)
      br label %exit
    exit:
      ret void
    }
    define void @file_may_leak(i8* %path, i1 %condition) {
      %file = call i8* @fopen(i8* %path, i8* %path)
      br i1 %condition, label %release, label %exit
    release:
      %ignored = call i32 @fclose(i8* %file)
      br label %exit
    exit:
      ret void
    }
  )");
  EXPECT_TRUE(run("cpp/double-free").findings.empty());
  EXPECT_TRUE(run("cpp/memory-never-freed").findings.empty());
  EXPECT_TRUE(run("cpp/memory-may-not-be-freed").findings.empty());
  EXPECT_EQ(run("cpp/descriptor-never-closed").findings.size(), 1u);
  EXPECT_EQ(run("cpp/file-may-not-be-closed").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, NullResultChecksRespectDirectReturnSummaries) {
  load(R"(
    @object = global i8 0
    define i8* @maybe(i1 %condition) {
      %p = select i1 %condition, i8* @object, i8* null
      ret i8* %p
    }
    define i8 @read(i8* %p) {
      %v = load i8, i8* %p
      ret i8 %v
    }
    define i8 @bad(i1 %condition) {
      %p = call i8* @maybe(i1 %condition)
      %v = call i8 @read(i8* %p)
      ret i8 %v
    }
    define i8 @good(i1 %condition) {
      %p = call i8* @maybe(i1 %condition)
      %nonnull = icmp ne i8* %p, null
      br i1 %nonnull, label %read, label %exit
    read:
      %v = call i8 @read(i8* %p)
      ret i8 %v
    exit:
      ret i8 0
    }
  )");
  EXPECT_EQ(run("cpp/deref-null-result").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest,
       PublishingSlotBeforeAllocationSuppressesOwnershipClaims) {
  load(R"(
    declare i8* @malloc(i64)
    declare void @register_slot(i8**)
    declare void @callback()
    define void @published() {
      %slot = alloca i8*
      store i8* null, i8** %slot
      call void @register_slot(i8** %slot)
      %p = call i8* @malloc(i64 8)
      store i8* %p, i8** %slot
      call void @callback()
      ret void
    }
    define void @private() {
      %slot = alloca i8*
      store i8* null, i8** %slot
      %p = call i8* @malloc(i64 8)
      store i8* %p, i8** %slot
      ret void
    }
  )");
  auto result = run("cpp/memory-never-freed");
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_EQ(result.findings.front().site->getFunc()->getName(), "private");
}
TEST_F(PDGRuleEngineTest, StorageIdentityAndFreezeAreNotStructuralEquivalence) {
  load(R"(
    define i32 @identity(i32 %left, i32 %right) {
      %a = alloca i32
      %b = alloca i32
      store i32 %left, i32* %a
      store i32 %right, i32* %b
      %x = load i32, i32* %a
      %y = load i32, i32* %b
      %again = load i32, i32* %a
      %f1 = freeze i32 undef
      %f2 = freeze i32 undef
      %sum = add i32 %x, %y
      ret i32 %sum
    }
  )");
  auto *function = module->getFunction("identity");
  auto named = [&](StringRef name) -> Value * {
    for (auto &inst : function->getEntryBlock())
      if (inst.getName() == name)
        return &inst;
    return nullptr;
  };
  FunctionFacts facts(*function);
  EXPECT_FALSE(facts.equivalent(*named("a"), *named("b")));
  EXPECT_FALSE(facts.equivalent(*named("x"), *named("y")));
  EXPECT_TRUE(facts.equivalent(*named("x"), *named("again")));
  EXPECT_FALSE(facts.equivalent(*named("f1"), *named("f2")));
  EXPECT_TRUE(facts.equivalent(*named("f1"), *named("f1")));
}

TEST_F(PDGRuleEngineTest, RangeGuardsRequireTheControllingEdgeToDominateTheUse) {
  load(R"(
    declare i8* @malloc(i64)
    declare void @observe()
    define i8* @join_guard(i64 %n) {
      %large = icmp ugt i64 %n, 1024
      br i1 %large, label %observe, label %join
    observe:
      call void @observe()
      br label %join
    join:
      %allocation = call i8* @malloc(i64 %n)
      ret i8* %allocation
    }
    define i8* @exit_guard(i64 %n) {
      %large = icmp ugt i64 %n, 1024
      br i1 %large, label %exit, label %allocate
    exit:
      ret i8* null
    allocate:
      %allocation = call i8* @malloc(i64 %n)
      ret i8* %allocation
    }
  )");
  for (const char *name : {"join_guard", "exit_guard"}) {
    auto *function = module->getFunction(name);
    const Instruction *allocation = nullptr;
    for (const auto &block : *function)
      for (const auto &inst : block)
        if (inst.getName() == "allocation")
          allocation = &inst;
    ASSERT_NE(allocation, nullptr);
    EXPECT_EQ(
        FunctionFacts(*function).boundedAt(*function->getArg(0), *allocation),
        StringRef(name) == "exit_guard");
  }
}

TEST_F(PDGRuleEngineTest, ScalarAndMemoryTaintFactsKeepProgramPointSeparation) {
  load(R"(
    @name = private constant [2 x i8] c"X\00"
    @clean = private constant [3 x i8] c"ok\00"
    declare i8* @getenv(i8*)
    declare i32 @atoi(i8*)
    declare i8* @strcpy(i8*,i8*)
    declare i64 @strlen(i8*)
    define i32 @main() {
      %name = getelementptr [2 x i8], [2 x i8]* @name, i32 0, i32 0
      %input = call i8* @getenv(i8* %name)
      %number = call i32 @atoi(i8* %input)
      %sum = add i32 %number, 1
      %buffer = alloca [3 x i8]
      %p = getelementptr [3 x i8], [3 x i8]* %buffer, i32 0, i32 0
      %copy = call i8* @strcpy(i8* %p,i8* %input)
      %before = call i64 @strlen(i8* %p)
      %literal = getelementptr [3 x i8], [3 x i8]* @clean, i32 0, i32 0
      %overwrite = call i8* @strcpy(i8* %p,i8* %literal)
      %after = call i64 @strlen(i8* %p)
      ret i32 %sum
    }
  )");
  auto *function = module->getFunction("main");
  auto named = [&](StringRef name) -> Instruction * {
    for (auto &inst : function->getEntryBlock())
      if (inst.getName() == name)
        return &inst;
    return nullptr;
  };
  auto result = TaintAnalysis(graph).analyze(*module);
  EXPECT_FALSE(result.origins(*named("sum")).empty());
  EXPECT_FALSE(result.originsAt(*named("before"), *named("p")).empty());
  EXPECT_TRUE(result.originsAt(*named("after"), *named("p")).empty());
}

TEST_F(PDGRuleEngineTest, WordexpFlagsRemainConditionalAcrossWrapperCalls) {
  load(R"(
    @name = private constant [2 x i8] c"X\00"
    declare i8* @getenv(i8*)
    declare i32 @wordexp(i8*,i8*,i32)
    define void @expand(i8* %input, i32 %flags) {
      %result = call i32 @wordexp(i8* %input,i8* null,i32 %flags)
      ret void
    }
    define i32 @main(i32 %flags) {
      %name = getelementptr [2 x i8], [2 x i8]* @name, i32 0, i32 0
      %input = call i8* @getenv(i8* %name)
      %bad = call i32 @wordexp(i8* %input,i8* null,i32 0)
      %good = call i32 @wordexp(i8* %input,i8* null,i32 4)
      %safe_flags = or i32 %flags, 4
      %safe_dynamic = call i32 @wordexp(i8* %input,i8* null,i32 %safe_flags)
      call void @expand(i8* %input, i32 0)
      call void @expand(i8* %input, i32 4)
      ret i32 0
    }
  )");
  auto result = run("cpp/wordexp-injection");
  ASSERT_EQ(result.findings.size(), 2u);
  for (const auto &finding : result.findings)
    EXPECT_EQ(finding.site->getFunc()->getName(), "main");
}

TEST_F(PDGRuleEngineTest, MemoryPhiDiamondsShareDefinitionTraversal) {
  std::string ir = R"(
    target datalayout = "e-p:64:64-i32:32"
    declare i8* @malloc(i64)
    define i8* @diamonds(i32 %n, i32 %m, i1 %condition) {
    entry:
      %source = alloca i32
      %other = alloca i32
      store i32 %n, i32* %source
      br label %guard0
  )";
  for (unsigned index = 0; index < 40; ++index) {
    std::string id = std::to_string(index);
    ir += "guard" + id + ":\n  br i1 %condition, label %left" + id +
          ", label %right" + id + "\n";
    ir += "left" + id + ":\n  store i32 1, i32* %other\n  br label %join" + id +
          "\n";
    ir += "right" + id + ":\n  store i32 2, i32* %other\n  br label %join" +
          id + "\n";
    ir += "join" + id + ":\n";
    if (index < 39)
      ir += "  br label %guard" + std::to_string(index + 1) + "\n";
  }
  ir += R"(
      %read = load i32, i32* %source
      %product = mul i32 %read, %m
      %size = zext i32 %product to i64
      %allocation = call i8* @malloc(i64 %size)
      ret i8* %allocation
    }
  )";
  load(ir.c_str());
  auto begin = std::chrono::steady_clock::now();
  auto result = run("cpp/multiplication-overflow-in-alloc");
  EXPECT_EQ(result.findings.size(), 1u);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(2));
}

TEST_F(PDGRuleEngineTest, WrapperRolesFollowContentWritesAndLocalSources) {
  load(R"(
    @name = private constant [2 x i8] c"X\00"
    @clean = private constant [3 x i8] c"ok\00"
    declare i8* @getenv(i8*)
    declare i8* @strcpy(i8*,i8*)
    declare i32 @wordexp(i8*,i8*,i32)
    define void @clear(i8* %input) {
      store i8 0, i8* %input
      %result = call i32 @wordexp(i8* %input,i8* null,i32 0)
      ret void
    }
    define void @overwrite(i8* %input) {
      %name = getelementptr [2 x i8], [2 x i8]* @name, i32 0, i32 0
      %source = call i8* @getenv(i8* %name)
      %copied = call i8* @strcpy(i8* %input,i8* %source)
      %result = call i32 @wordexp(i8* %input,i8* null,i32 0)
      ret void
    }
    define i32 @main() {
      %name = getelementptr [2 x i8], [2 x i8]* @name, i32 0, i32 0
      %input = call i8* @getenv(i8* %name)
      call void @clear(i8* %input)
      %buffer = alloca [8 x i8]
      %p = getelementptr [8 x i8], [8 x i8]* %buffer, i32 0, i32 0
      %clean = getelementptr [3 x i8], [3 x i8]* @clean, i32 0, i32 0
      %copied = call i8* @strcpy(i8* %p,i8* %clean)
      call void @overwrite(i8* %p)
      ret i32 0
    }
  )");
  auto result = run("cpp/wordexp-injection");
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_EQ(result.findings.front().site->getFunc()->getName(), "overwrite");
}
TEST_F(PDGRuleEngineTest, PointerOriginsKeepLiveOnEntryAndClobbersUnknown) {
  load(R"(
    @outside = external global i8*
    declare void @opaque(i8**)
    define void @aliases(i8* %p) {
      %slot = alloca i8*
      store i8* %p, i8** %slot
      %known = load i8*, i8** %slot
      %incoming = load i8*, i8** @outside
      call void @opaque(i8** %slot)
      %clobbered = load i8*, i8** %slot
      ret void
    }
  )");
  auto *function = module->getFunction("aliases");
  auto named = [&](StringRef name) -> Instruction * {
    for (auto &inst : function->getEntryBlock())
      if (inst.getName() == name)
        return &inst;
    return nullptr;
  };
  FunctionFacts facts(*function);
  EXPECT_EQ(&facts.pointerOrigin(*named("known")), function->getArg(0));
  EXPECT_EQ(&facts.pointerOrigin(*named("incoming")), named("incoming"));
  EXPECT_EQ(&facts.pointerOrigin(*named("clobbered")), named("clobbered"));
}

TEST_F(PDGRuleEngineTest,
       StandardStringContentIsSeparateFromObjectLayoutAndRefs) {
  load(R"(
    target datalayout = "e-m:e-p:64:64-i64:64"
    %"class.std::basic_string" = type { i64, i64, i64 }
    %Pair = type { %"class.std::basic_string", %"class.std::basic_string" }
    @name = private constant [2 x i8] c"X\00"
    @clean = private constant [3 x i8] c"ok\00"
    declare i8* @getenv(i8*)
    declare i32 @system(i8*)
    declare void @_ZNSt12basic_stringIcSt11char_traitsIcESaIcEEC1EPKc(
        %"class.std::basic_string"*, i8*)
    declare %"class.std::basic_string"* @_ZNSt12basic_stringIcSt11char_traitsIcESaIcEE6assignEPKc(
        %"class.std::basic_string"*, i8*)
    declare i8* @_ZNKSt12basic_stringIcSt11char_traitsIcESaIcEE5c_strEv(
        %"class.std::basic_string"*)
    define void @clear_helper(%"class.std::basic_string"* %object, i8* %literal) {
      %slot = alloca %"class.std::basic_string"*
      store %"class.std::basic_string"* %object, %"class.std::basic_string"** %slot
      %receiver = load %"class.std::basic_string"*, %"class.std::basic_string"** %slot
      %assigned = call %"class.std::basic_string"* @_ZNSt12basic_stringIcSt11char_traitsIcESaIcEE6assignEPKc(
          %"class.std::basic_string"* %receiver, i8* %literal)
      ret void
    }
    define i32 @main() {
      %name = getelementptr [2 x i8], [2 x i8]* @name, i32 0, i32 0
      %input = call i8* @getenv(i8* %name)
      %clean = getelementptr [3 x i8], [3 x i8]* @clean, i32 0, i32 0
      %pair = alloca %Pair
      %left = getelementptr %Pair, %Pair* %pair, i32 0, i32 0
      %right = getelementptr %Pair, %Pair* %pair, i32 0, i32 1
      call void @_ZNSt12basic_stringIcSt11char_traitsIcESaIcEEC1EPKc(
          %"class.std::basic_string"* %left, i8* %input)
      call void @_ZNSt12basic_stringIcSt11char_traitsIcESaIcEEC1EPKc(
          %"class.std::basic_string"* %right, i8* %clean)
      %left_data = call i8* @_ZNKSt12basic_stringIcSt11char_traitsIcESaIcEE5c_strEv(
          %"class.std::basic_string"* %left)
      %bad = call i32 @system(i8* %left_data)
      %raw = bitcast %"class.std::basic_string"* %left to i8*
      %byte = load i8, i8* %raw
      %right_data = call i8* @_ZNKSt12basic_stringIcSt11char_traitsIcESaIcEE5c_strEv(
          %"class.std::basic_string"* %right)
      %isolated = call i32 @system(i8* %right_data)
      %reference = call %"class.std::basic_string"* @_ZNSt12basic_stringIcSt11char_traitsIcESaIcEE6assignEPKc(
          %"class.std::basic_string"* %left, i8* %input)
      call void @clear_helper(%"class.std::basic_string"* %left,i8* %clean)
      %ref_data = call i8* @_ZNKSt12basic_stringIcSt11char_traitsIcESaIcEE5c_strEv(
          %"class.std::basic_string"* %reference)
      %cleared = call i32 @system(i8* %ref_data)
      ret i32 0
    }
  )");
  auto result = run("cpp/uncontrolled-process-operation");
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_EQ(result.findings.front().site->getValue()->getName(), "bad");
  auto facts = TaintAnalysis(graph).analyze(*module);
  for (auto &inst : module->getFunction("main")->getEntryBlock())
    if (inst.getName() == "byte")
      EXPECT_TRUE(facts.origins(inst).empty());
}

TEST_F(PDGRuleEngineTest,
       LibrarySymbolEscapesFollowTargetManglingAndExactAliases) {
  load(R"(
    target datalayout = "e-m:o-p:64:64"
    target triple = "arm64-apple-darwin"
    declare i32 @"\01_system"(i8*)
    declare i32 @_system(i8*)
    declare i8* @strcpy(i8*,i8*)
    declare i8* @stpcpy(i8*,i8*)
    define void @calls(i8* %destination,i8* %source) {
      %copy = call i8* @strcpy(i8* %destination,i8* %source)
      %end = call i8* @stpcpy(i8* %destination,i8* %source)
      ret void
    }
  )");
  EXPECT_TRUE(
      ValueFacts::hasLibraryName(*module->getFunction("\1_system"), "system"));
  EXPECT_FALSE(
      ValueFacts::hasLibraryName(*module->getFunction("_system"), "system"));
  for (auto &inst : module->getFunction("calls")->getEntryBlock())
    if (auto *call = dyn_cast<CallBase>(&inst)) {
      if (inst.getName() == "copy")
        EXPECT_EQ(LibraryModels::returnAlias(*call), Optional<unsigned>(0));
      if (inst.getName() == "end")
        EXPECT_FALSE(LibraryModels::returnAlias(*call));
    }
}

TEST_F(PDGRuleEngineTest, ApiConfigurationChecksRequireActualUnsafeSettings) {
  load(R"(
    declare i32 @EVP_PKEY_CTX_set_rsa_keygen_bits(i8*,i32)
    declare i32 @SetSecurityDescriptorDacl(i8*,i32,i8*,i32)
    declare i8* @xmlReadMemory(i8*,i32,i8*,i8*,i32)
    define void @config(i8* %context,i8* %data) {
      %weak = call i32 @EVP_PKEY_CTX_set_rsa_keygen_bits(i8* %context,i32 1024)
      %good = call i32 @EVP_PKEY_CTX_set_rsa_keygen_bits(i8* %context,i32 2048)
      %unknown = call i32 @EVP_PKEY_CTX_set_rsa_keygen_bits(i8* %context,i32 0)
      %bad_dacl = call i32 @SetSecurityDescriptorDacl(i8* %context,i32 1,i8* null,i32 0)
      %absent_dacl = call i32 @SetSecurityDescriptorDacl(i8* %context,i32 0,i8* null,i32 0)
      %xml_bad = call i8* @xmlReadMemory(i8* %data,i32 8,i8* null,i8* null,i32 2)
      %xml_good = call i8* @xmlReadMemory(i8* %data,i32 8,i8* null,i8* null,i32 0)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/insufficient-key-size").findings.size(), 1u);
  EXPECT_EQ(run("cpp/unsafe-dacl-security-descriptor").findings.size(), 1u);
  EXPECT_EQ(run("cpp/external-entity-expansion").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest, FormatAbiAndSentinelConventionsKeepSafeCallsSeparate) {
  load(R"(
    target datalayout = "e-m:e-p:64:64-i64:64"
    target triple = "x86_64-unknown-linux-gnu"
    @format = private constant [3 x i8] c"%s\00"
    @star = private constant [4 x i8] c"%*s\00"
    declare i32 @printf(i8*,...)
    declare void @collect(i32,...)
    define void @calls(i8* %text) {
      %fmt = getelementptr [3 x i8], [3 x i8]* @format,i32 0,i32 0
      %star = getelementptr [4 x i8], [4 x i8]* @star,i32 0,i32 0
      %bad = call i32 (i8*,...) @printf(i8* %fmt,i32 5)
      %good = call i32 (i8*,...) @printf(i8* %fmt,i8* %text)
      %bad_width = call i32 (i8*,...) @printf(i8* %star,double 2.0,i8* %text)
      call void (i32,...) @collect(i32 1,i8* %text,i8* null)
      call void (i32,...) @collect(i32 2,i8* %text,i8* null)
      call void (i32,...) @collect(i32 3,i8* %text,i8* null)
      call void (i32,...) @collect(i32 4,i8* %text,i8* null)
      call void (i32,...) @collect(i32 5,i8* %text)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/wrong-type-format-argument").findings.size(), 2u);
  EXPECT_EQ(run("cpp/unterminated-variadic-call").findings.size(), 1u);
}

TEST_F(PDGRuleEngineTest,
       ReallocZeroThroughAStackSlotDoesNotInventFailureLeaks) {
  load(R"(
    declare i8* @malloc(i64)
    declare i8* @realloc(i8*,i64)
    declare void @free(i8*)
    define void @zero() {
      %size_slot = alloca i64
      store i64 0,i64* %size_slot
      %p = call i8* @malloc(i64 8)
      %size = load i64,i64* %size_slot
      %r = call i8* @realloc(i8* %p,i64 %size)
      %null = icmp eq i8* %r,null
      br i1 %null,label %exit,label %release
    release:
      call void @free(i8* %r)
      br label %exit
    exit:
      ret void
    }
  )");
  EXPECT_TRUE(
      run("cpp/memory-leak-on-failed-call-to-realloc").findings.empty());
  EXPECT_TRUE(run("cpp/memory-may-not-be-freed").findings.empty());
}

TEST_F(PDGRuleEngineTest, ScanfWritesInvalidateOldNullAndConstantFacts) {
  load(R"(
    @fmt_pointer = private constant [3 x i8] c"%p\00"
    @fmt_integer = private constant [3 x i8] c"%d\00"
    declare i32 @scanf(i8*,...)
    define i32 @pointer() {
      %slot = alloca i32*
      store i32* null,i32** %slot
      %format = getelementptr [3 x i8],[3 x i8]* @fmt_pointer,i32 0,i32 0
      %rc = call i32 (i8*,...) @scanf(i8* %format,i32** %slot)
      %success = icmp eq i32 %rc,1
      br i1 %success,label %read,label %exit
    read:
      %p = load i32*,i32** %slot
      %value = load i32,i32* %p
      ret i32 %value
    exit:
      ret i32 0
    }
    define i32 @scalar() {
      %slot = alloca i32
      store i32 1,i32* %slot
      %format = getelementptr [3 x i8],[3 x i8]* @fmt_integer,i32 0,i32 0
      %rc = call i32 (i8*,...) @scanf(i8* %format,i32* %slot)
      %value = load i32,i32* %slot
      %changed = icmp eq i32 %value,0
      br i1 %changed,label %read,label %exit
    read:
      %bad = load i32,i32* null
      ret i32 %bad
    exit:
      ret i32 0
    }
  )");
  auto result = run("cpp/missing-null-test");
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_EQ(result.findings.front().site->getFunc()->getName(), "scalar");
}

TEST_F(PDGRuleEngineTest, LifetimeBudgetDoesNotReduceTheIndependentTaintBudget) {
  load(R"(
    @name = private constant [2 x i8] c"X\00"
    declare i8* @getenv(i8*)
    declare i8* @malloc(i64)
    declare i32 @wordexp(i8*,i8*,i32)
    define i32 @main() {
      %name = getelementptr [2 x i8],[2 x i8]* @name,i32 0,i32 0
      %input = call i8* @getenv(i8* %name)
      %bad = call i32 @wordexp(i8* %input,i8* null,i32 0)
      %allocation = call i8* @malloc(i64 8)
      br label %exit
    exit:
      ret i32 0
    }
  )");
  RuleEnginePolicy policy;
  policy.lifetime_states_per_object = 1;
  auto result = RuleEngine(graph).analyze(
      {"cpp/wordexp-injection", "cpp/memory-never-freed"}, {}, {}, module.get(),
      {}, policy);
  ASSERT_TRUE(result.diagnostics.state_limit_hit);
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_EQ(result.findings.front().rule_id, "cpp/wordexp-injection");
  for (const auto &note : result.diagnostics.notes)
    EXPECT_EQ(note.find("Taint step limit reached"), std::string::npos);
}
} // namespace
