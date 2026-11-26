#include <gtest/gtest.h>
#include <Dataflow/IFDS/Analyses/DefaultReachableAllocationSitesIDEProblem.h>
#include <Dataflow/IFDS/Analyses/IDEExtendedTaintAnalysis.h>
#include <Dataflow/IFDS/Analyses/IDEFeatureTaintAnalysis.h>
#include <Dataflow/IFDS/Analyses/IDEGeneralizedLCA.h>
#include <Dataflow/IFDS/Analyses/IDEInstInteractionAnalysis.h>
#include <Dataflow/IFDS/Analyses/IDESecureHeapPropagation.h>
#include <Dataflow/IFDS/Analyses/IDETypeState.h>
#include <Dataflow/IFDS/Solver/IDESolver.h>
#include <TestUtils/LLVMHelpers.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <set>

namespace ifds {
namespace {

struct ReachableFactValue {
  bool reachable = false;

  static ReachableFactValue top() { return ReachableFactValue{true}; }
  static ReachableFactValue bottom() { return ReachableFactValue{false}; }

  bool operator==(const ReachableFactValue &other) const {
    return reachable == other.reachable;
  }
};

class ReachableAllocationProblem
    : public DefaultReachableAllocationSitesIDEProblem<ReachableFactValue> {
public:
  Fact zero_fact() const override { return nullptr; }
  FactSet initial_facts(const llvm::Function *) override { return {zero_fact()}; }
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override {
    IDEInitialSeeds seeds;
    const llvm::Function *main = module.getFunction("main");
    if (!main) {
      return seeds;
    }
    const llvm::Instruction *seed_inst =
        lotus::unittest::findInstructionByName(*main, "p");
    if (seed_inst) {
      seeds.add_seed(seed_inst, seed_inst, ReachableFactValue::top());
    }
    return seeds;
  }

  ReachableFactValue top_value() const override {
    return ReachableFactValue::top();
  }
  ReachableFactValue bottom_value() const override {
    return ReachableFactValue::bottom();
  }
  ReachableFactValue join(const ReachableFactValue &lhs,
                          const ReachableFactValue &rhs) const override {
    return ReachableFactValue{lhs.reachable || rhs.reachable};
  }

  EdgeFunction normal_edge_function(const llvm::Instruction *,
                                    const llvm::Instruction *, const Fact &,
                                    const Fact &) override {
    return identity();
  }
  EdgeFunction call_edge_function(const llvm::CallBase *, const llvm::Function *,
                                  const Fact &, const Fact &) override {
    return identity();
  }
  EdgeFunction return_edge_function(const llvm::CallBase *,
                                    const llvm::Function *,
                                    const llvm::Instruction *,
                                    const llvm::Instruction *, const Fact &,
                                    const Fact &) override {
    return identity();
  }
  EdgeFunction call_to_return_edge_function(
      const llvm::CallBase *, const llvm::Instruction *,
      llvm::ArrayRef<const llvm::Function *>, const Fact &, const Fact &)
      override {
    return identity();
  }
};

class FileHandleDescription : public TypeStateDescriptionBase {
public:
  bool is_factory_function(llvm::StringRef func_name) const override {
    return func_name == "file_open";
  }
  bool is_consuming_function(llvm::StringRef func_name) const override {
    return func_name == "file_use" || func_name == "file_close";
  }
  std::string get_type_name_of_interest() const override {
    return "FileHandle";
  }
  std::set<int>
  get_consumer_param_indices(llvm::StringRef /*func_name*/) const override {
    return {0};
  }
  StateId uninitialized_state() const override { return 0; }
  StateId error_state() const override { return 3; }
  StateId get_next_state(llvm::StringRef token, StateId current_state,
                         const llvm::CallBase * /*call_site*/) const override {
    if (token == "file_open") {
      return 1;
    }
    if (token == "file_use") {
      return current_state == 2 ? 3 : current_state;
    }
    if (token == "file_close") {
      return current_state == 1 ? 2 : 3;
    }
    return current_state;
  }
};

class IDENewAnalysesTest : public ::testing::Test {
protected:
  void SetUp() override { Ctx = std::make_unique<llvm::LLVMContext>(); }
  std::unique_ptr<llvm::LLVMContext> Ctx;
};

TEST_F(IDENewAnalysesTest, ExtendedTaintMarksSourceCallResultTainted) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    declare i8* @recv()

    define i32 @main() {
    entry:
      %recv_val = call i8* @recv()
      ret i32 0
    }
  )", "ide_ext_taint");
  auto *Recv = M->getFunction("recv");
  auto *CallRecv = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*M->getFunction("main"),
                                             "recv_val"));

  IDEExtendedTaintAnalysis Problem;
  auto SF = Problem.summary_flow(CallRecv, Recv, Problem.zero_fact());
  EXPECT_TRUE(SF.count(CallRecv) > 0);

  auto EF = Problem.summary_edge_function(CallRecv, Recv, CallRecv,
                                          Problem.zero_fact(), CallRecv);
  auto V = EF(Problem.bottom_value());
  EXPECT_EQ(V.kind, ExtendedTaintValue::Tainted);
}

TEST_F(IDENewAnalysesTest, FeatureTaintAssignsSourceFeatureBit) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    declare i8* @recv()

    define i32 @main() {
    entry:
      %recv_val = call i8* @recv()
      ret i32 0
    }
  )", "ide_feature_taint");
  auto *Recv = M->getFunction("recv");
  auto *CallRecv = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*M->getFunction("main"),
                                             "recv_val"));

  IDEFeatureTaintAnalysis Problem;
  auto SF = Problem.summary_flow(CallRecv, Recv, Problem.zero_fact());
  EXPECT_TRUE(SF.count(CallRecv) > 0);

  auto EF = Problem.summary_edge_function(CallRecv, Recv, CallRecv,
                                          Problem.zero_fact(), CallRecv);
  auto V = EF(Problem.bottom_value());
  EXPECT_EQ(V.kind, FeatureTaintValue::Features);
  EXPECT_NE(V.mask & (1ull << 0), 0ull);
}

TEST_F(IDENewAnalysesTest, FeatureTaintAcceptsCallbackFeatureSets) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    declare i8* @read_secret()

    define i32 @main() {
    entry:
      %secret = call i8* @read_secret()
      ret i32 0
    }
  )", "ide_feature_taint_callback");
  auto *Read = M->getFunction("read_secret");
  auto *CallRead = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*M->getFunction("main"),
                                             "secret"));

  IDEFeatureTaintAnalysis Problem(
      [](FeatureTaintGenerator::InstOrGlobal value) {
        const auto *inst = std::get<const llvm::Instruction *>(value);
        return inst && inst->getName() == "secret";
      },
      [](FeatureTaintGenerator::InstOrGlobal) {
        return std::set<std::string>{"network", "tenant"};
      });
  auto SF = Problem.summary_flow(CallRead, Read, Problem.zero_fact());
  ASSERT_TRUE(SF.count(CallRead) > 0);
  auto EF = Problem.summary_edge_function(CallRead, Read, CallRead,
                                          Problem.zero_fact(), CallRead);
  auto V = EF(Problem.bottom_value());
  EXPECT_TRUE(V.has_feature("network"));
  EXPECT_TRUE(V.has_feature("tenant"));
}

TEST_F(IDENewAnalysesTest, FeatureTaintSeedsCallbackGlobals) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    @config = global i8 0

    define i32 @main() {
    entry:
      ret i32 0
    }
  )", "ide_feature_taint_global_callback");
  auto *Config = M->getNamedGlobal("config");
  ASSERT_NE(Config, nullptr);

  IDEFeatureTaintAnalysis Problem(
      [](FeatureTaintGenerator::InstOrGlobal value) {
        const auto *global =
            std::get<const llvm::GlobalVariable *>(value);
        return global && global->getName() == "config";
      },
      [](FeatureTaintGenerator::InstOrGlobal) {
        return std::set<std::string>{"configuration"};
      });

  const auto Seeds = Problem.initial_ide_seeds(*M);
  ASSERT_EQ(Seeds.get_seeds().size(), 1u);
  const auto &facts = Seeds.get_seeds().begin()->second;
  const auto fact = facts.find(Config);
  ASSERT_NE(fact, facts.end());
  EXPECT_TRUE(fact->second.has_feature("configuration"));
}

TEST_F(IDENewAnalysesTest, SecureHeapMarksAllocatorResultAllocated) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    declare i8* @malloc(i64)

    define i32 @main() {
    entry:
      %buf = call i8* @malloc(i64 16)
      ret i32 0
    }
  )", "ide_secure_heap");
  auto *Malloc = M->getFunction("malloc");
  auto *CallMalloc = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*M->getFunction("main"), "buf"));

  IDESecureHeapPropagation Problem;
  auto SF = Problem.summary_flow(CallMalloc, Malloc, Problem.zero_fact());
  EXPECT_TRUE(SF.count(CallMalloc) > 0);

  auto EF = Problem.summary_edge_function(CallMalloc, Malloc, CallMalloc,
                                          Problem.zero_fact(), CallMalloc);
  auto V = EF(Problem.bottom_value());
  EXPECT_EQ(V.kind, SecureHeapValue::Allocated);
}

TEST_F(IDENewAnalysesTest, InstInteractionMarksLoadAsRead) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    define i32 @main(i32* %ptr) {
    entry:
      %loaded = load i32, i32* %ptr
      ret i32 %loaded
    }
  )", "ide_inst_interaction");
  auto *Main = M->getFunction("main");
  auto *Load = lotus::unittest::findInstructionByName(*Main, "loaded");
  auto *Ret = Main->back().getTerminator();

  IDEInstInteractionAnalysis Problem;
  IDESolver<IDEInstInteractionAnalysis> Solver(Problem);
  Solver.solve(*M);

  auto V = Solver.get_value_at(Ret, Load);
  EXPECT_TRUE(V.kind == InstInteractionValue::Read ||
              V.kind == InstInteractionValue::ReadWrite);
}

TEST_F(IDENewAnalysesTest, GeneralizedLCAComputesConstantSet) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    define i32 @main() {
    entry:
      %sum = add i32 3, 4
      ret i32 %sum
    }
  )", "ide_glca");
  auto *Main = M->getFunction("main");
  auto *Add = lotus::unittest::findInstructionByName(*Main, "sum");
  auto *Ret = Main->back().getTerminator();

  IDEGeneralizedLCA Problem;
  IDESolver<IDEGeneralizedLCA> Solver(Problem);
  Solver.solve(*M);

  auto V = Solver.get_value_at(Ret, Add);
  EXPECT_EQ(V.kind, GLCAValue::ConstantSet);
  EXPECT_EQ(V.constants.size(), 1u);
  EXPECT_TRUE(V.constants.count(7) > 0);
}

TEST_F(IDENewAnalysesTest,
       ReachableAllocationSitesPropagateThroughStoreLoadAndCalls) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    declare i8* @malloc(i64)

    define i8* @id(i8* %arg) {
    entry:
      ret i8* %arg
    }

    define i32 @main() {
    entry:
      %p = call i8* @malloc(i64 4)
      %slot = alloca i8*
      store i8* %p, i8** %slot
      %tmp = load i8*, i8** %slot
      %q = call i8* @id(i8* %tmp)
      ret i32 0
    }
  )", "ide_reachable_alloc_sites");
  auto *Main = M->getFunction("main");
  auto *Q = lotus::unittest::findInstructionByName(*Main, "q");
  auto *Ret = Main->back().getTerminator();

  ReachableAllocationProblem Problem;
  IDESolver<ReachableAllocationProblem> Solver(Problem);
  auto Config = Solver.get_solver_config();
  Config.set_auto_inject_alias_analysis(true);
  Solver.set_solver_config(Config);
  Solver.solve(*M);

  auto V = Solver.get_value_at(Ret, Q);
  EXPECT_TRUE(V.reachable);
}

TEST_F(IDENewAnalysesTest, DescriptionDrivenTypeStateUsesSummaryTransitions) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    %FileHandle = type { i32 }

    declare %FileHandle* @file_open()
    declare void @file_use(%FileHandle*)
    declare void @file_close(%FileHandle*)

    define i32 @main() {
    entry:
      %fh = call %FileHandle* @file_open()
      call void @file_use(%FileHandle* %fh)
      call void @file_close(%FileHandle* %fh)
      ret i32 0
    }
  )", "ide_typestate_description");
  auto *Main = M->getFunction("main");
  auto *Handle = lotus::unittest::findInstructionByName(*Main, "fh");
  auto *Ret = Main->back().getTerminator();

  IDETypeState Problem(std::make_shared<FileHandleDescription>());
  IDESolver<IDETypeState> Solver(Problem);
  Solver.solve(*M);

  auto V = Solver.get_value_at(Ret, Handle);
  ASSERT_FALSE(V.is_special());
  EXPECT_EQ(V.user_state(), 2);
}

} // namespace
} // namespace ifds

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
