// Models adapted from PhASAR 88366fc69, lib/Utils/LibCSummary.cpp.
// Copyright (c) PhASAR contributors. MIT: third-party/phasar/LICENSE.txt.
#include "Dataflow/IFDS/Analyses/IFDSTaintAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

namespace ifds {
namespace {

class LibCSummaryTest : public ::testing::Test {
protected:
  llvm::LLVMContext context;
  std::unique_ptr<TaintConfig> model;

  void SetUp() override {
    model = TaintConfigParser::parse_file(std::string(LOTUS_PHASAR_CONFIG_DIR) +
                                          "/taint.spec");
    ASSERT_NE(model, nullptr);
  }

  TaintAnalysis::FactSet flow(TaintAnalysis &problem, llvm::CallBase *call,
                              TaintFact fact) {
    return problem.call_to_return_flow(call, call->getNextNode(),
                                       {call->getCalledFunction()}, fact);
  }
};

TEST_F(LibCSummaryTest, SearchPreservesHaystackDataWithoutCopyingNeedleData) {
  struct Case {
    const char *name;
    const char *element;
    const char *parameters;
    const char *arguments;
    bool memory_selector;
  };
  for (const auto &test :
       {Case{"memmem", "i8", "i8*, i64, i8*, i64",
             "i8* %src, i64 16, i8* %needle, i64 2", true},
        Case{"memrchr", "i8", "i8*, i32, i64", "i8* %src, i32 %ch, i64 16",
             false},
        Case{"rawmemchr", "i8", "i8*, i32", "i8* %src, i32 %ch", false},
        Case{"strchrnul", "i8", "i8*, i32", "i8* %src, i32 %ch", false},
        Case{"index", "i8", "i8*, i32", "i8* %src, i32 %ch", false},
        Case{"rindex", "i8", "i8*, i32", "i8* %src, i32 %ch", false},
        Case{"wcschr", "i32", "i32*, i32", "i32* %src, i32 %ch", false},
        Case{"wcsrchr", "i32", "i32*, i32", "i32* %src, i32 %ch", false},
        Case{"wcspbrk", "i32", "i32*, i32*", "i32* %src, i32* %needle", true},
        Case{"wcsstr", "i32", "i32*, i32*", "i32* %src, i32* %needle", true},
        Case{"wmemchr", "i32", "i32*, i32, i64", "i32* %src, i32 %ch, i64 16",
             false}}) {
    SCOPED_TRACE(test.name);
    const std::string pointer = std::string(test.element) + "*";
    auto module = lotus::unittest::parseModuleChecked(
        context,
        "declare " + pointer + " @" + test.name + "(" + test.parameters +
            ")\ndefine void @main(" + pointer + " %src, " + pointer +
            " %needle, i32 %ch) {\n"
            "%result = call " +
            pointer + " @" + test.name + "(" + test.arguments +
            ")\nret void\n}",
        "libc_search");
    auto *main = module->getFunction("main");
    auto *call = llvm::cast<llvm::CallBase>(
        lotus::unittest::findInstructionByName(*main, "result"));
    TaintAnalysis problem(TaintAnalysis::Config{}, *model);
    const auto data =
        flow(problem, call, TaintFact::tainted_memory(main->getArg(0)));
    EXPECT_TRUE(data.count(TaintFact::tainted_memory(call)));
    EXPECT_TRUE(data.count(TaintFact::tainted_var(call)));
    const auto address =
        flow(problem, call, TaintFact::tainted_var(main->getArg(0)));
    EXPECT_TRUE(address.count(TaintFact::tainted_var(call)));
    EXPECT_FALSE(address.count(TaintFact::tainted_memory(call)));
    const auto selector =
        flow(problem, call,
             test.memory_selector ? TaintFact::tainted_memory(main->getArg(1))
                                  : TaintFact::tainted_var(main->getArg(2)));
    EXPECT_TRUE(selector.count(TaintFact::tainted_var(call)));
    EXPECT_FALSE(selector.count(TaintFact::tainted_memory(call)));
    EXPECT_FALSE(selector.count(TaintFact::tainted_memory(main->getArg(0))));
    EXPECT_EQ(flow(problem, call, TaintFact::zero()).size(), 1U);
  }
}

TEST_F(LibCSummaryTest, DecompositionWritesOutputsFromScalarInputOnly) {
  for (const std::string name : {"modf", "modff", "modfl", "frexpf", "frexpl",
                                 "sincos", "sincosf", "sincosl"}) {
    SCOPED_TRACE(name);
    const bool two_outputs = name.find("sincos") == 0;
    const bool exponent = name.find("frexp") == 0;
    const std::string scalar = name.back() == 'f' && name != "modf" ? "float"
                               : name.back() == 'l'                 ? "x86_fp80"
                                                                    : "double";
    const std::string out = exponent ? "i32" : scalar;
    const std::string result = two_outputs ? "void" : scalar;
    auto module = lotus::unittest::parseModuleChecked(
        context,
        "declare " + result + " @" + name + "(" + scalar + ", " + out + "*" +
            (two_outputs ? ", " + out + "*" : "") + ")\ndefine void @main(" +
            scalar + " %x, " + out + "* %out, " + out + "* %other) {\n" +
            (two_outputs ? "" : "%result = ") + "call " + result + " @" + name +
            "(" + scalar + " %x, " + out + "* %out" +
            (two_outputs ? ", " + out + "* %other" : "") + ")\nret void\n}",
        "libc_decomposition");
    auto *main = module->getFunction("main");
    auto *call = llvm::cast<llvm::CallBase>(&main->front().front());
    TaintAnalysis problem(TaintAnalysis::Config{}, *model);
    const auto facts =
        flow(problem, call, TaintFact::tainted_var(main->getArg(0)));
    EXPECT_TRUE(facts.count(TaintFact::tainted_memory(main->getArg(1))));
    EXPECT_EQ(facts.count(TaintFact::tainted_memory(main->getArg(2))),
              two_outputs ? 1U : 0U);
    EXPECT_EQ(facts.count(TaintFact::tainted_var(call)), two_outputs ? 0U : 1U);
    EXPECT_FALSE(flow(problem, call, TaintFact::tainted_memory(main->getArg(1)))
                     .count(TaintFact::tainted_var(call)));
  }
}

TEST_F(LibCSummaryTest, BufferTransformsUseCorrectDirectionAndReturnKind) {
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    declare i8* @memccpy(i8*, i8*, i32, i64)
    declare i32* @wmemset(i32*, i32, i64)
    declare i64 @strxfrm(i8*, i8*, i64)
    declare i64 @wcsxfrm(i32*, i32*, i64)
    declare i8* @inet_ntop(i32, i8*, i8*, i32)
    declare i32 @inet_pton(i32, i8*, i8*)
    define void @main(i8* %src, i8* %dst, i32* %wsrc, i32* %wdst, i32 %ch) {
      %copy = call i8* @memccpy(i8* %dst, i8* %src, i32 0, i64 16)
      %fill = call i32* @wmemset(i32* %wdst, i32 %ch, i64 16)
      %collate = call i64 @strxfrm(i8* %dst, i8* %src, i64 16)
      %wcollate = call i64 @wcsxfrm(i32* %wdst, i32* %wsrc, i64 16)
      %text = call i8* @inet_ntop(i32 2, i8* %src, i8* %dst, i32 16)
      %binary = call i32 @inet_pton(i32 2, i8* %src, i8* %dst)
      ret void
    })",
                                                    "libc_buffers");
  auto *main = module->getFunction("main");
  TaintAnalysis problem(TaintAnalysis::Config{}, *model);
  for (const std::string name :
       {"copy", "fill", "collate", "wcollate", "text", "binary"}) {
    SCOPED_TRACE(name);
    auto *call = llvm::cast<llvm::CallBase>(
        lotus::unittest::findInstructionByName(*main, name));
    const bool wide = name == "fill" || name == "wcollate";
    const auto *dst = main->getArg(wide ? 3 : 1);
    const auto *src = main->getArg(wide ? 2 : 0);
    const auto input = name == "fill" ? TaintFact::tainted_var(main->getArg(4))
                                      : TaintFact::tainted_memory(src);
    const auto facts = flow(problem, call, input);
    EXPECT_TRUE(facts.count(TaintFact::tainted_memory(dst)));
    const bool pointer_result = call->getType()->isPointerTy();
    EXPECT_EQ(facts.count(TaintFact::tainted_memory(call)),
              pointer_result ? 1U : 0U);
    if (!pointer_result)
      EXPECT_TRUE(facts.count(TaintFact::tainted_var(call)));
    EXPECT_FALSE(flow(problem, call, TaintFact::tainted_memory(dst))
                     .count(TaintFact::tainted_memory(src)));
    if (pointer_result) {
      const auto address = flow(problem, call, TaintFact::tainted_var(dst));
      EXPECT_TRUE(address.count(TaintFact::tainted_var(call)));
      EXPECT_FALSE(address.count(TaintFact::tainted_memory(call)));
    }
  }
}

TEST_F(LibCSummaryTest, FilledBufferTaintSurvivesSearchAndLoad) {
  model->sources.insert("source");
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    declare i32 @source()
    declare i32* @wmemset(i32*, i32, i64)
    declare i32* @wmemchr(i32*, i32, i64)
    declare void @sink(i32)
    define void @main() {
      %buffer = alloca i32
      %clean = alloca i32
      %input = call i32 @source()
      call i32* @wmemset(i32* %buffer, i32 %input, i64 1)
      %found = call i32* @wmemchr(i32* %buffer, i32 65, i64 1)
      %value = load i32, i32* %found
      call void @sink(i32 %value)
      call i32* @wmemset(i32* %clean, i32 65, i64 1)
      %clean_found = call i32* @wmemchr(i32* %clean, i32 65, i64 1)
      %clean_value = load i32, i32* %clean_found
      call void @sink(i32 %clean_value)
      ret void
    })",
                                                    "libc_fill_search");
  auto *main = module->getFunction("main");
  auto *value = lotus::unittest::findInstructionByName(*main, "value");
  auto *clean = lotus::unittest::findInstructionByName(*main, "clean_value");
  for (bool sparse : {false, true}) {
    SCOPED_TRACE(sparse);
    TaintAnalysis problem(TaintAnalysis::Config{}, *model);
    IFDSSolver<TaintAnalysis> solver(problem);
    solver.get_solver_config().set_sparse_execution(sparse);
    solver.solve(*module);
    EXPECT_TRUE(solver.get_facts_at_entry(value->getNextNode())
                    .count(TaintFact::tainted_var(value)));
    EXPECT_FALSE(solver.get_facts_at_entry(clean->getNextNode())
                     .count(TaintFact::tainted_var(clean)));
  }
}

} // namespace
} // namespace ifds
