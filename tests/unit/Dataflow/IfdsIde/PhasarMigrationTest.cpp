// Regression scenarios adapted from PhASAR 88366fc69, including
// typestate_analysis_fileio/typestate_{01,02,05,06}.c and the OpenSSL
// protocol tests. Copyright (c) 2020 Philipp Schubert and others.
// MIT; see third-party/phasar/LICENSE.txt. Fixtures use LLVM 14 typed pointers
// and declarations only, so tests do not require libc or OpenSSL development
// files.
#include "Dataflow/IFDS/Analyses/IDEConstantPropagation.h"
#include "Dataflow/IFDS/Analyses/IFDSTaintAnalysis.h"
#include "Dataflow/IFDS/Analyses/TypeStateDescriptions.h"
#include "Dataflow/IFDS/Solver/IDESolver.h"
#include "TestUtils/LLVMHelpers.h"

#include <stdexcept>

#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace ifds {
namespace {
class PhasarMigrationTest : public ::testing::Test {
protected:
  llvm::LLVMContext context;
  TypeStateValue
  runProtocol(std::shared_ptr<TypeStateDescriptionBase> description,
              const std::string &ir, bool sparse = false) {
    auto module =
        lotus::unittest::parseModuleChecked(context, ir, "phasar_protocol");
    auto *main = module->getFunction("main");
    auto *object = lotus::unittest::findInstructionByName(*main, "object");
    EXPECT_NE(object, nullptr);
    IDETypeState problem(std::move(description));
    IDESolver<IDETypeState> solver(problem);
    auto config = solver.get_solver_config();
    config.set_sparse_execution(sparse);
    solver.set_solver_config(config);
    solver.solve(*module);
    return solver.get_value_at(main->back().getTerminator(), object);
  }
  std::string fileProgram(const std::string &body) {
    return R"(
      %struct._IO_FILE = type { i32 }
      declare %struct._IO_FILE* @fopen(i8*, i8*)
      declare i32 @fclose(%struct._IO_FILE*)
      declare i64 @fread(i8*, i64, i64, %struct._IO_FILE*)
      define i32 @main(i1 %condition) {
      entry:
        %object = call %struct._IO_FILE* @fopen(i8* null, i8* null)
    )" + body +
           "\n ret i32 0\n}\n";
  }
};

TEST_F(PhasarMigrationTest, FileOpenAndClose) {
  for (bool sparse : {false, true}) {
    auto state = runProtocol(
        std::make_shared<CSTDFILEIOTypeStateDescription>(),
        fileProgram("call i32 @fclose(%struct._IO_FILE* %object)"), sparse);
    ASSERT_FALSE(state.is_special());
    EXPECT_EQ(state.user_state(), CSTDFILEIOTypeStateDescription::Closed);
  }
}
TEST_F(PhasarMigrationTest, FileRemainsOpenWithoutClose) {
  auto state = runProtocol(std::make_shared<CSTDFILEIOTypeStateDescription>(),
                           fileProgram(""));
  ASSERT_FALSE(state.is_special());
  EXPECT_EQ(state.user_state(), CSTDFILEIOTypeStateDescription::Opened);
}
TEST_F(PhasarMigrationTest, FileDoubleCloseAndReadAfterClose) {
  for (const std::string operation :
       {"call i32 @fclose(%struct._IO_FILE* %object)",
        "call i64 @fread(i8* null, i64 1, i64 1, %struct._IO_FILE* %object)"}) {
    auto state = runProtocol(
        std::make_shared<CSTDFILEIOTypeStateDescription>(),
        fileProgram("call i32 @fclose(%struct._IO_FILE* %object)\n" +
                    operation));
    ASSERT_FALSE(state.is_special());
    EXPECT_EQ(state.user_state(), CSTDFILEIOTypeStateDescription::Error);
  }
}
TEST_F(PhasarMigrationTest, FileConditionalCloseJoinsToUnknown) {
  auto state = runProtocol(std::make_shared<CSTDFILEIOTypeStateDescription>(),
                           fileProgram(R"(
      br i1 %condition, label %close, label %exit
    close:
      call i32 @fclose(%struct._IO_FILE* %object)
      br label %exit
    exit:
    )"));
  EXPECT_TRUE(state.is_top());
}
TEST_F(PhasarMigrationTest, FileClosingOneHandlePreservesOther) {
  auto state = runProtocol(std::make_shared<CSTDFILEIOTypeStateDescription>(),
                           fileProgram(R"(
      %other = call %struct._IO_FILE* @fopen(i8* null, i8* null)
      call i32 @fclose(%struct._IO_FILE* %other)
    )"));
  ASSERT_FALSE(state.is_special());
  EXPECT_EQ(state.user_state(), CSTDFILEIOTypeStateDescription::Opened);
}
TEST_F(PhasarMigrationTest, FileArgumentPositionsAndUnknownOperations) {
  CSTDFILEIOTypeStateDescription description;
  EXPECT_EQ(description.get_consumer_param_indices("fread"),
            std::set<int>({3}));
  EXPECT_EQ(description.get_consumer_param_indices("fgets"),
            std::set<int>({2}));
  EXPECT_EQ(description.get_consumer_param_indices("fputs"),
            std::set<int>({1}));
  EXPECT_EQ(description.get_factory_param_indices("fdopen"),
            std::set<int>({-1}));
  EXPECT_FALSE(description.is_api_function("freopen"));
  EXPECT_EQ(description.get_next_state("unrelated", 1, nullptr), 1);
  EXPECT_EQ(description.get_next_state("fclose", -1, nullptr),
            description.error_state());
  EXPECT_EQ(description.get_next_state("fclose", 42, nullptr),
            description.error_state());
}
TEST_F(PhasarMigrationTest, KdfFetchFreeAndDoubleFree) {
  const std::string declarations = R"(
    %struct.evp_kdf_st = type { i32 }
    declare %struct.evp_kdf_st* @EVP_KDF_fetch(i8*, i8*, i8*)
    declare void @EVP_KDF_free(%struct.evp_kdf_st*)
    define i32 @main() {
      %object = call %struct.evp_kdf_st* @EVP_KDF_fetch(i8* null, i8* null, i8* null)
      call void @EVP_KDF_free(%struct.evp_kdf_st* %object)
  )";
  for (bool twice : {false, true}) {
    auto state = runProtocol(
        std::make_shared<OpenSSLEVPKDFDescription>(),
        declarations +
            (twice ? "call void @EVP_KDF_free(%struct.evp_kdf_st* %object)"
                   : "") +
            "\nret i32 0\n}");
    ASSERT_FALSE(state.is_special());
    EXPECT_EQ(state.user_state(),
              twice ? OpenSSLEVPKDFDescription::Error
                    : OpenSSLEVPKDFDescription::Uninitialized);
  }
}
TEST_F(PhasarMigrationTest, SecureMemoryMustBeCleansedBeforeFree) {
  const std::string prefix = R"(
    declare i8* @CRYPTO_malloc(i64, i8*, i32)
    declare void @OPENSSL_cleanse(i8*, i64)
    declare void @CRYPTO_free(i8*, i8*, i32)
    define i32 @main() {
      %object = call i8* @CRYPTO_malloc(i64 32, i8* null, i32 0)
  )";
  for (bool cleanse : {false, true}) {
    auto state = runProtocol(
        std::make_shared<OpenSSLSecureMemoryDescription>(),
        prefix +
            (cleanse ? "call void @OPENSSL_cleanse(i8* %object, i64 32)\n"
                     : "") +
            "call void @CRYPTO_free(i8* %object, i8* null, i32 0)\nret i32 "
            "0\n}");
    ASSERT_FALSE(state.is_special());
    EXPECT_EQ(state.user_state(), cleanse
                                      ? OpenSSLSecureMemoryDescription::Freed
                                      : OpenSSLSecureMemoryDescription::Error);
  }
}
TEST_F(PhasarMigrationTest, KdfContextRequiresFetchedKdfAndParameters) {
  for (bool fetched : {false, true}) {
    for (bool params : {false, true}) {
      auto desc = std::make_shared<OpenSSLEVPKDFCTXDescription>(
          [fetched](const llvm::CallBase *) { return fetched; });
      auto state = runProtocol(
          desc,
          std::string(R"(
        %struct.evp_kdf_ctx_st = type { i32 }
        declare %struct.evp_kdf_ctx_st* @EVP_KDF_CTX_new(i8*)
        declare i32 @EVP_KDF_CTX_set_params(%struct.evp_kdf_ctx_st*, i8*)
        declare i32 @EVP_KDF_derive(%struct.evp_kdf_ctx_st*, i8*, i64, i8*)
        define i32 @main() {
          %object = call %struct.evp_kdf_ctx_st* @EVP_KDF_CTX_new(i8* null)
      )") +
              (params
                   ? "call i32 @EVP_KDF_CTX_set_params(%struct.evp_kdf_ctx_st* "
                     "%object, i8* null)\n"
                   : "") +
              "call i32 @EVP_KDF_derive(%struct.evp_kdf_ctx_st* %object, i8* "
              "null, i64 32, i8* null)\nret i32 0\n}");
      ASSERT_FALSE(state.is_special());
      EXPECT_EQ(state.user_state(), fetched && params
                                        ? OpenSSLEVPKDFCTXDescription::Derived
                                        : OpenSSLEVPKDFCTXDescription::Error);
    }
  }
}
TEST_F(PhasarMigrationTest, SecureHeapRequiresInitializationAndClearFree) {
  for (bool initialized : {false, true}) {
    for (bool clear : {false, true}) {
      auto desc = std::make_shared<OpenSSLSecureHeapDescription>(
          [initialized](const llvm::CallBase *) { return initialized; });
      auto state = runProtocol(
          desc, std::string(R"(
        declare i8* @CRYPTO_secure_malloc(i64, i8*, i32)
        declare void @CRYPTO_secure_free(i8*, i8*, i32)
        declare void @CRYPTO_secure_clear_free(i8*, i64, i8*, i32)
        define i32 @main() {
          %object = call i8* @CRYPTO_secure_malloc(i64 32, i8* null, i32 0)
      )") +
                    (clear ? "call void @CRYPTO_secure_clear_free(i8* %object, "
                             "i64 32, i8* null, i32 0)\n"
                           : "call void @CRYPTO_secure_free(i8* %object, i8* "
                             "null, i32 0)\n") +
                    "ret i32 0\n}");
      ASSERT_FALSE(state.is_special());
      EXPECT_EQ(state.user_state(), initialized && clear
                                        ? OpenSSLSecureHeapDescription::Freed
                                        : OpenSSLSecureHeapDescription::Error);
    }
  }
}
TEST_F(PhasarMigrationTest, PrerequisiteQueriesCannotBeOmitted) {
  EXPECT_THROW(OpenSSLEVPKDFCTXDescription({}), std::invalid_argument);
  EXPECT_THROW(OpenSSLSecureHeapDescription({}), std::invalid_argument);
}

TEST_F(PhasarMigrationTest, LibcModelsPreserveValueAndMemoryDirections) {
  auto model = TaintConfigParser::parse_file(
      std::string(LOTUS_PHASAR_CONFIG_DIR) + "/taint.spec");
  ASSERT_NE(model, nullptr);
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    declare double @hypot(double, double)
    declare float @strtof(i8*, i8**)
    declare i8* @stpcpy(i8*, i8*)
    define i32 @main(double %x, double %y, i8* %src, i8* %dst, i8** %end) {
      %math = call double @hypot(double %x, double %y)
      %number = call float @strtof(i8* %src, i8** %end)
      %copy = call i8* @stpcpy(i8* %dst, i8* %src)
      ret i32 0
    })",
                                                    "libc_models");
  auto *main = module->getFunction("main");
  auto *math = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*main, "math"));
  auto *number = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*main, "number"));
  auto *copy = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*main, "copy"));
  TaintAnalysis problem(TaintAnalysis::Config{}, *model);
  auto flow = [&](llvm::CallBase *call, TaintFact fact) {
    const llvm::Function *callee = call->getCalledFunction();
    return problem.call_to_return_flow(call, call->getNextNode(), {callee},
                                       fact);
  };
  EXPECT_TRUE(flow(math, TaintFact::tainted_var(main->getArg(0)))
                  .count(TaintFact::tainted_var(math)));
  EXPECT_TRUE(flow(math, TaintFact::tainted_var(main->getArg(1)))
                  .count(TaintFact::tainted_var(math)));
  EXPECT_TRUE(flow(number, TaintFact::tainted_memory(main->getArg(2)))
                  .count(TaintFact::tainted_var(number)));
  EXPECT_TRUE(flow(number, TaintFact::tainted_var(main->getArg(2)))
                  .count(TaintFact::tainted_memory(main->getArg(4))));
  EXPECT_TRUE(flow(copy, TaintFact::tainted_memory(main->getArg(2)))
                  .count(TaintFact::tainted_memory(main->getArg(3))));
  EXPECT_FALSE(flow(copy, TaintFact::tainted_memory(main->getArg(3)))
                   .count(TaintFact::tainted_memory(main->getArg(2))));
  for (const auto &name :
       {"copysignf", "fdim", "fdimf", "fdiml", "nextafter", "nextafterl",
        "nexttoward", "nexttowardf", "nexttowardl", "scalblnf"}) {
    auto *spec = model->get_function_config(name);
    ASSERT_NE(spec, nullptr) << name;
    ASSERT_EQ(spec->pipe_specs.size(), 2U) << name;
    EXPECT_EQ(spec->pipe_specs[1].from.arg_index, 1) << name;
  }
  EXPECT_TRUE(model->is_source("getenv"));
  EXPECT_TRUE(model->is_sink("system"));
}
// Adapted from linear_constant/basic_02.cpp: a later store replaces 13 by 17.
TEST_F(PhasarMigrationTest, ConstantOverwriteThroughMemory) {
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    define i32 @main() {
      %slot = alloca i32
      store i32 13, i32* %slot
      store i32 17, i32* %slot
      %value = load i32, i32* %slot
      ret i32 %value
    })",
                                                    "constant_overwrite");
  auto *main = module->getFunction("main");
  auto *value = lotus::unittest::findInstructionByName(*main, "value");
  for (bool sparse : {false, true}) {
    IDEConstantPropagation problem;
    IDESolver<IDEConstantPropagation> solver(problem);
    auto config = solver.get_solver_config();
    config.set_sparse_execution(sparse);
    solver.set_solver_config(config);
    solver.solve(*module);
    auto result = solver.get_value_at(main->back().getTerminator(), value);
    EXPECT_EQ(result.kind, LCPValue::Const);
    EXPECT_EQ(result.value, 17);
  }
}

TEST_F(PhasarMigrationTest, LibcMathTaintReachesSinkThroughWrapper) {
  auto model = TaintConfigParser::parse_file(
      std::string(LOTUS_PHASAR_CONFIG_DIR) + "/taint.spec");
  ASSERT_NE(model, nullptr);
  model->sources.insert("source");
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    declare double @source()
    declare double @hypot(double, double)
    declare void @sink(double)
    define double @wrapper(double %arg) {
      %computed = call double @hypot(double %arg, double 0.0)
      ret double %computed
    }
    define i32 @main() {
      %input = call double @source()
      %result = call double @wrapper(double %input)
      call void @sink(double %result)
      ret i32 0
    })",
                                                    "libc_math_taint");
  auto *main = module->getFunction("main");
  auto *result = lotus::unittest::findInstructionByName(*main, "result");
  auto *sink = result->getNextNode();
  for (bool sparse : {false, true}) {
    TaintAnalysis problem(TaintAnalysis::Config{}, *model);
    IFDSSolver<TaintAnalysis> solver(problem);
    solver.get_solver_config().set_sparse_execution(sparse);
    solver.solve(*module);
    EXPECT_TRUE(
        solver.get_facts_at_entry(sink).count(TaintFact::tainted_var(result)));
  }
}
} // namespace
} // namespace ifds
