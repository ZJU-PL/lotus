// DFI regression and differential tests. Protocol follows PhASAR 88366fc69's
// DFISolverTest; LLVM fixtures and client adapters target Lotus/LLVM 14.
#include "Dataflow/IFDS/Solver/DFI/DFISolver.h"

#include "Dataflow/IFDS/Analyses/IFDSTaintAnalysis.h"
#include "Dataflow/IFDS/Analyses/IFDSUninitializedVariables.h"
#include "Dataflow/IFDS/Solver/IFDSSolver.h"
#include "TestUtils/LLVMHelpers.h"

#include <map>
#include <random>
#include <sstream>
#include <tuple>

#include <gtest/gtest.h>
#include <llvm/IR/InstIterator.h>

namespace ifds {
namespace {
class IntegerProblem : public IFDSProblem<int> {
public:
  bool auto_zero = false, bypass = true, collapse = false, split = false;
  bool generate = false, kill_all = false, fail = false;
  std::string seed_function = "main";
  FactSet seed_facts{1};
  bool saw_unknown = false;
  std::map<
      std::tuple<const llvm::Instruction *, const llvm::Instruction *, int>,
      unsigned>
      normal_calls;
  int zero_fact() const override { return 0; }
  bool auto_add_zero() const override { return auto_zero; }
  FactSet initial_facts(const llvm::Function *) override { return seed_facts; }
  InitialSeeds initial_seeds(const llvm::Module &module) override {
    InitialSeeds seeds;
    auto *function = module.getFunction(seed_function);
    if (function && !function->empty())
      seeds.add_seed(&function->getEntryBlock().front(), seed_facts);
    return seeds;
  }
  FactSet normal_flow(const llvm::Instruction *inst,
                      const llvm::Instruction *succ, const int &fact) override {
    ++normal_calls[{inst, succ, fact}];
    if (fail)
      throw std::runtime_error("controlled transfer failure");
    if (kill_all)
      return {};
    FactSet out{fact};
    if (generate && inst->getOpcode() == llvm::Instruction::Add)
      out.insert(3);
    if (split && inst->isTerminator())
      out.insert(succ->getParent()->getName() == "left" ? 7 : 8);
    return out;
  }
  FactSet call_flow(const llvm::CallBase *, const llvm::Function *,
                    const int &fact) override {
    return collapse && fact != 0 ? FactSet{9} : FactSet{fact};
  }
  FactSet return_flow(const llvm::CallBase *call, const llvm::Instruction *,
                      const llvm::Instruction *site, const llvm::Function *,
                      const int &exit, const int &caller) override {
    FactSet result{collapse && call->getFunction()->getName() == "main"
                       ? 100 + caller
                       : exit};
    if (split)
      result.insert(site->getParent()->getName() == "left" ? 17 : 18);
    return result;
  }
  FactSet call_to_return_flow(const llvm::CallBase *, const llvm::Instruction *,
                              llvm::ArrayRef<const llvm::Function *> callees,
                              const int &fact) override {
    FactSet result = bypass ? FactSet{fact} : FactSet{};
    for (auto *callee : callees)
      if (!callee) {
        saw_unknown = true;
        result.insert(99);
      }
    return result;
  }
  FactSet summary_flow(const llvm::CallBase *, const llvm::Function *callee,
                       const int &) override {
    return callee && callee->getName() == "modeled" ? FactSet{42} : FactSet{};
  }
};

class DFISolverTest : public ::testing::Test {
protected:
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> parse(const std::string &ir) {
    return lotus::unittest::parseModuleChecked(context, ir, "dfi_test");
  }
  template <typename Problem, typename Config>
  void compare(const llvm::Module &module, IFDSSolver<Problem> &reference,
               DFIIFDSSolver<Problem, Config> &dfi) {
    reference.solve(module);
    dfi.solve(module);
    for (const auto &function : module)
      for (const auto &inst : llvm::instructions(function)) {
        EXPECT_EQ(reference.get_facts_at_entry(&inst),
                  dfi.get_facts_at_entry(&inst))
            << function.getName().str() << ": " << inst.getOpcodeName();
        EXPECT_EQ(reference.get_facts_at_exit(&inst),
                  dfi.get_facts_at_exit(&inst));
        EXPECT_EQ(reference.get_facts_at_in_llvm_ssa(&inst),
                  dfi.get_facts_at_in_llvm_ssa(&inst));
      }
    std::vector<SummaryEdge<typename Problem::FactType>> a, b;
    reference.get_summary_edges(a);
    dfi.get_summary_edges(b);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    EXPECT_EQ(a, b);
  }
  template <typename Config = DFIConfig>
  void check(const llvm::Module &module, IntegerProblem a = {},
             IFDSIDESolverConfig config = {}) {
    IntegerProblem b = a;
    IFDSSolver<IntegerProblem> reference(a);
    DFIIFDSSolver<IntegerProblem, Config> dfi(b);
    reference.set_solver_config(config);
    dfi.set_solver_config(config);
    compare(module, reference, dfi);
    for (const auto &entry : b.normal_calls)
      EXPECT_EQ(entry.second, 1U); // one evaluation per (inst, succ, fact)
  }
};
TEST_F(DFISolverTest, BranchLoopGenerationAndNoIndexAgreeWithTabulation) {
  auto module = parse(R"(
    define i32 @main(i1 %cond) {
    entry: br label %loop
    loop:
      %sum = add i32 1, 2
      br i1 %cond, label %left, label %right
    left: br label %loop
    right: ret i32 0
    })");
  IntegerProblem problem;
  problem.generate = true;
  problem.split = true;
  check(*module, problem);
  check<DFIConfigBackward>(*module, problem);
  check<DFIConfigNoIndex>(*module, problem);
}
TEST_F(DFISolverTest, ZeroInjectionAndZeroDisabled) {
  auto module = parse(R"(
    define i32 @main() {
      %a = add i32 1, 2
      %b = add i32 3, 4
      ret i32 0
    })");
  IntegerProblem problem;
  problem.kill_all = true;
  check(*module, problem);
  problem.auto_zero = true;
  check(*module, problem);
  IntegerProblem empty;
  empty.seed_facts.clear();
  check(*module, empty);
  empty.auto_zero = true;
  check(*module, empty);
}
TEST_F(DFISolverTest, MultipleCallersRetainCallerFactsAndSummaryReuse) {
  auto module = parse(R"(
    define void @callee() { ret void }
    define i32 @main() {
      call void @callee()
      %between = add i32 1, 2
      call void @callee()
      ret i32 0
    })");
  IntegerProblem a, b;
  a.bypass = b.bypass = false;
  a.collapse = b.collapse = true;
  a.seed_facts = b.seed_facts = {1, 2};
  IFDSSolver<IntegerProblem> reference(a);
  DFISolver<IntegerProblem> dfi(b);
  compare(*module, reference, dfi);
  auto *main = module->getFunction("main");
  EXPECT_EQ(dfi.get_facts_at_entry(main->back().getTerminator()),
            IntegerProblem::FactSet({201, 202}));
  EXPECT_FALSE(dfi.get_facts_at_entry(main->back().getTerminator()).count(109));
}
TEST_F(DFISolverTest, DirectAndMutualRecursionConverge) {
  auto module = parse(R"(
    define void @f(i1 %c) {
      br i1 %c, label %base, label %recurse
    base: ret void
    recurse:
      call void @g(i1 %c)
      ret void
    }
    define void @g(i1 %c) {
      call void @f(i1 %c)
      ret void
    }
    define i32 @main(i1 %c) {
      call void @f(i1 %c)
      ret i32 0
    })");
  IntegerProblem problem;
  problem.bypass = false;
  check(*module, problem);
  check<DFIConfigBackward>(*module, problem);
  auto *g = module->getFunction("g");
  auto *call = llvm::cast<llvm::CallBase>(&g->getEntryBlock().front());
  call->setCalledFunction(g);
  // A non-returning recursive SCC must converge without inventing returns.
  check(*module, problem);
}
TEST_F(DFISolverTest, AdditiveSummariesDoNotSuppressBodies) {
  auto module = parse(R"(
    declare void @external()
    define void @modeled() { ret void }
    define i32 @main() {
      call void @modeled()
      call void @external()
      ret i32 0
    })");
  IntegerProblem problem;
  check(*module, problem);
  IntegerProblem copy;
  DFISolver<IntegerProblem> solver(copy);
  solver.solve(*module);
  EXPECT_TRUE(solver
                  .get_facts_at_entry(
                      module->getFunction("main")->back().getTerminator())
                  .count(42));
  EXPECT_TRUE(
      solver
          .get_facts_at_entry(&module->getFunction("modeled")->front().front())
          .count(1));
}
TEST_F(DFISolverTest, PartialIndirectTargetsAndCustomIcfg) {
  auto module = parse(R"(
    define void @f() { ret void }
    define void @g() { ret void }
    define i32 @main(void ()* %fp) {
      call void %fp()
      ret i32 0
    })");
  auto provider = [&module](const llvm::CallBase &) {
    return CalleeTargets{{module->getFunction("f"), module->getFunction("g")},
                         false};
  };
  IntegerProblem a, b;
  IFDSSolver<IntegerProblem> reference(a);
  DFISolver<IntegerProblem> dfi(b);
  reference.set_callee_provider(provider);
  dfi.set_callee_provider(provider);
  compare(*module, reference, dfi);
  EXPECT_TRUE(a.saw_unknown);
  EXPECT_TRUE(b.saw_unknown);
  auto cfg = std::make_shared<dataflow::controlflow::LLVMInterCFG>(
      module.get(), [&module](llvm::Instruction *) {
        return std::vector<llvm::Function *>{module->getFunction("f")};
      });
  reference.set_callee_provider({});
  dfi.set_callee_provider({});
  reference.set_icfg(cfg);
  dfi.set_icfg(cfg);
  compare(*module, reference, dfi);
}
TEST_F(DFISolverTest, InvokeKeepsBothReturnSitesAndSuccessorSpecificFlows) {
  auto module = parse(R"(
    declare i32 @__gxx_personality_v0(...)
    define i32 @callee() { ret i32 7 }
    define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
      %v = invoke i32 @callee() to label %left unwind label %right
    left: ret i32 %v
    right:
      %lp = landingpad {i8*, i32} cleanup
      ret i32 0
    })");
  IntegerProblem problem;
  problem.split = true;
  problem.bypass = false;
  check(*module, problem);
  problem.auto_zero = true;
  check(*module, problem);
}
TEST_F(DFISolverTest, ReturnsPastSeedsAcrossMultipleFunctions) {
  auto module = parse(R"(
    define void @leaf() { ret void }
    define void @middle() {
      call void @leaf()
      ret void
    }
    define i32 @main() {
      call void @middle()
      ret i32 0
    })");
  IntegerProblem problem;
  problem.seed_function = "leaf";
  check(*module, problem);
  IFDSIDESolverConfig config;
  config.set_follow_returns_past_seeds();
  check(*module, problem, config);
  check<DFIConfigBackward>(*module, problem, config);
  problem.auto_zero = true;
  check(*module, problem, config);
}
TEST_F(DFISolverTest, ResultsSnapshotsNoIndexAndRepeatedSolve) {
  auto module = parse(R"(
    define i32 @main() {
      %v = add i32 1, 2
      ret i32 0
    })");
  IntegerProblem problem;
  DFISolver<IntegerProblem> dfi(problem);
  EXPECT_THROW(dfi.get_results(), std::logic_error);
  EXPECT_THROW(dfi.get_reachability(), std::logic_error);
  dfi.solve(*module);
  auto old = dfi.get_results();
  auto query = dfi.get_reachability();
  auto *ret = module->getFunction("main")->back().getTerminator();
  problem.seed_facts = {2};
  dfi.solve(*module);
  EXPECT_EQ(old.get_facts_at_entry(ret), IntegerProblem::FactSet{1});
  EXPECT_EQ(dfi.get_facts_at_entry(ret), IntegerProblem::FactSet{2});
  EXPECT_TRUE(query.find(ret, 1));
  DFISolver<IntegerProblem, DFIConfigNoIndex> no_index(problem);
  no_index.solve(*module);
  EXPECT_EQ(no_index.get_facts_at_entry(ret), IntegerProblem::FactSet{2});
  EXPECT_THROW(no_index.get_reachability(), std::logic_error);
  problem.fail = true;
  EXPECT_THROW(dfi.solve(*module), std::runtime_error);
  EXPECT_THROW(dfi.get_results(), std::logic_error);
  problem.fail = false;
  dfi.solve(*module);
  EXPECT_EQ(dfi.get_facts_at_entry(ret), IntegerProblem::FactSet{2});
}

template <typename Config> void check_queries(llvm::Module &module) {
  IntegerProblem problem;
  problem.bypass = false;
  DFISolver<IntegerProblem, Config> solver(problem);
  solver.solve(module);
  auto results = solver.get_results();
  auto query = solver.get_reachability();
  auto *main = module.getFunction("main");
  auto *start = &main->front().front();
  auto *finish = main->back().getTerminator();
  auto *inside = module.getFunction("callee")->back().getTerminator();
  EXPECT_TRUE(query.reaches(start, 1, finish, 1));
  EXPECT_TRUE(query.reaches(start, 1, inside, 1));
  EXPECT_FALSE(
      query.reaches(inside, 1, finish, 1)); // no return above start context
  EXPECT_FALSE(query.reaches(start, 999, finish, 1));
  EXPECT_FALSE(query.reaches(UINT32_MAX, 0));
  std::vector<typename DFIResults<int>::Node> nodes;
  results.for_each_result([&](const llvm::Instruction *inst, int fact) {
    nodes.emplace_back(inst, fact);
  });
  for (const auto &node : nodes) {
    auto id = query.find(node.instruction, node.fact);
    ASSERT_TRUE(id);
    EXPECT_TRUE(query.reaches(*id, *id));
    std::set<std::pair<const llvm::Instruction *, int>> descendants, ancestors;
    query.for_each_descendant(*id,
                              [&](const llvm::Instruction *inst, int fact) {
                                descendants.emplace(inst, fact);
                              });
    query.for_each_ancestor(*id, [&](const llvm::Instruction *inst, int fact) {
      ancestors.emplace(inst, fact);
    });
    for (const auto &other : nodes) {
      auto key = std::make_pair(other.instruction, other.fact);
      EXPECT_EQ(descendants.count(key) != 0,
                query.reaches(node.instruction, node.fact, other.instruction,
                              other.fact));
      EXPECT_EQ(ancestors.count(key) != 0,
                query.reaches(other.instruction, other.fact, node.instruction,
                              node.fact));
    }
  }
  auto start_id = *query.find(start, 1), end_id = *query.find(finish, 1);
  std::set<int> at_end, at_start;
  query.for_each_descendant_fact_at(start_id, finish,
                                    [&](int f) { at_end.insert(f); });
  query.for_each_ancestor_fact_at(end_id, start,
                                  [&](int f) { at_start.insert(f); });
  EXPECT_EQ(at_end, std::set<int>{1});
  EXPECT_EQ(at_start, std::set<int>{1});
}
TEST_F(DFISolverTest,
       ForwardAndBackwardQueriesMatchEnumerationAndContextBoundary) {
  auto module = parse(R"(
    define void @callee() { ret void }
    define i32 @main() {
      call void @callee()
      %mid = add i32 1, 2
      call void @callee()
      ret i32 0
    })");
  check_queries<DFIConfig>(*module);
  check_queries<DFIConfigBackward>(*module);
}
TEST_F(DFISolverTest, RandomFiniteCfgsMatchTabulation) {
  std::mt19937 random(21371);
  for (unsigned trial = 0; trial < 30; ++trial) {
    std::ostringstream ir;
    ir << "define i32 @main(i1 %c) { entry: br label %b0\n";
    for (unsigned block = 0; block < 12; ++block) {
      ir << "b" << block << ": %v" << block << " = add i32 1, 2\n";
      if (block == 11)
        ir << "ret i32 0\n";
      else
        ir << "br i1 %c, label %b" << (block + 1) << ", label %b"
           << random() % 12 << "\n";
    }
    ir << "}\n";
    auto module = parse(ir.str());
    IntegerProblem problem;
    problem.generate = true;
    problem.auto_zero = trial % 2;
    check(*module, problem);
  }
}
TEST_F(DFISolverTest, TaintClientDenseSparseAndAliasInjection) {
  auto module = parse(R"(
    declare void @sink(i8*)
    declare i32 @scalar_source()
    define i32 @main(i8* %input) {
      %slot = alloca i8*
      %scalar = call i32 @scalar_source()
      store i8* %input, i8** %slot
      %a = add i32 1, 2
      %b = add i32 3, 4
      %c = add i32 5, 6
      %value = load i8*, i8** %slot
      call void @sink(i8* %value)
      ret i32 0
    })");
  TaintAnalysis::Config taint_config;
  taint_config.taint_main_args = true;
  TaintConfig model;
  model.sources.insert("scalar_source");
  TaintAnalysis a(taint_config, model), b(taint_config, model);
  IFDSSolver<TaintAnalysis> reference(a);
  DFISolver<TaintAnalysis> dfi(b);
  IFDSIDESolverConfig config;
  config.set_auto_inject_alias_analysis();
  reference.set_solver_config(config);
  dfi.set_solver_config(config);
  compare(*module, reference, dfi);
  config.set_sparse_execution();
  reference.set_solver_config(config);
  dfi.set_solver_config(config);
  compare(*module, reference, dfi);
  EXPECT_GT(dfi.get_sparse_transfers(), 0U);
  auto *value = lotus::unittest::findInstructionByName(
      *module->getFunction("main"), "value");
  bool tainted = false;
  for (const auto &fact : dfi.get_facts_at_entry(value->getNextNode()))
    tainted |= fact.is_tainted_var() && fact.get_value() == value;
  EXPECT_TRUE(tainted);
}
TEST_F(DFISolverTest, UninitializedClientMatchesFactsAndDiagnostics) {
  auto module = parse(R"(
    define i32 @read_slot(i32* %p) {
      %value = load i32, i32* %p
      ret i32 %value
    }
    define i32 @main() {
      %slot = alloca i32
      %unknown = call i32 @read_slot(i32* %slot)
      store i32 7, i32* %slot
      %known = load i32, i32* %slot
      ret i32 %known
    })");
  UninitializedVariablesAnalysis a, b;
  IFDSSolver<UninitializedVariablesAnalysis> reference(a);
  DFISolver<UninitializedVariablesAnalysis> dfi(b);
  compare(*module, reference, dfi);
  auto reports = [](const UninitializedVariablesAnalysis &problem) {
    std::set<std::pair<const llvm::Instruction *, const llvm::Value *>> found;
    for (const auto &report : problem.get_results())
      found.emplace(report.use_site, report.uninitialized_value);
    return found;
  };
  EXPECT_EQ(reports(a), reports(b));
}
class TwoSeedsProblem : public IntegerProblem {
public:
  InitialSeeds initial_seeds(const llvm::Module &module) override {
    InitialSeeds seeds;
    seeds.add_seed(&module.getFunction("main")->front().front(), 1);
    seeds.add_seed(&module.getFunction("leaf")->front().front(), 2);
    return seeds;
  }
};
TEST_F(DFISolverTest, BalancedAndUnbalancedSeedContextsCoexist) {
  auto module = parse(R"(
    define void @leaf() { ret void }
    define i32 @main() {
      call void @leaf()
      ret i32 0
    })");
  TwoSeedsProblem a, b;
  IFDSSolver<TwoSeedsProblem> reference(a);
  DFISolver<TwoSeedsProblem> dfi(b);
  reference.get_solver_config().set_follow_returns_past_seeds();
  dfi.get_solver_config().set_follow_returns_past_seeds();
  compare(*module, reference, dfi);
  auto *ret = module->getFunction("main")->back().getTerminator();
  EXPECT_EQ(dfi.get_facts_at_entry(ret), IntegerProblem::FactSet({1, 2}));
  auto *leaf_exit = module->getFunction("leaf")->back().getTerminator();
  EXPECT_TRUE(dfi.get_reachability().reaches(leaf_exit, 2, ret, 2));
}
TEST_F(DFISolverTest, SnapshotsOutliveSolverAndOwnedAliasIsReleased) {
  auto module = parse("define i32 @main() { %v = add i32 1, 2 ret i32 0 }");
  IntegerProblem problem;
  auto results = [&]() {
    DFISolver<IntegerProblem> solver(problem);
    solver.get_solver_config().set_auto_inject_alias_analysis();
    solver.solve(*module);
    EXPECT_TRUE(problem.has_alias_analysis_configured());
    return solver.get_results();
  }();
  EXPECT_FALSE(problem.has_alias_analysis_configured());
  auto *ret = module->getFunction("main")->back().getTerminator();
  EXPECT_EQ(results.get_facts_at_entry(ret), IntegerProblem::FactSet{1});
}
TEST_F(DFISolverTest, SessionEntryPointsAndNoSeedsAreHonored) {
  auto module = parse(R"(
    define i32 @entry(i8* %input) { ret i32 0 }
    define i32 @main() { ret i32 0 }
    )");
  AnalysisSession::Options options;
  options.call_graph = lotus::CallGraphAnalysisType::CHA;
  options.entry_points = {"entry"};
  auto session = std::make_shared<AnalysisSession>(*module, options);
  TaintConfig model;
  TaintAnalysis a(TaintAnalysis::Config{}, model),
      b(TaintAnalysis::Config{}, model);
  IFDSSolver<TaintAnalysis> reference(a);
  DFISolver<TaintAnalysis> dfi(b);
  reference.set_analysis_session(session);
  dfi.set_analysis_session(session);
  compare(*module, reference, dfi);
  EXPECT_FALSE(
      dfi.get_facts_at_entry(&module->getFunction("entry")->front().front())
          .empty());
  EXPECT_TRUE(
      dfi.get_facts_at_entry(&module->getFunction("main")->front().front())
          .empty());
  IntegerProblem none;
  none.seed_function = "missing";
  DFISolver<IntegerProblem> empty(none);
  empty.solve(*module);
  EXPECT_EQ(empty.get_results().num_nodes(), 0U);
  EXPECT_EQ(empty.get_statistics().expanded_vertices, 0U);
  EXPECT_FALSE(empty.get_reachability().find(
      &module->getFunction("main")->front().front(), 1));
}
TEST_F(DFISolverTest, SharedSessionCustomGraphAndForeignSessionValidation) {
  auto module = parse(R"(
    define void @callee() { ret void }
    define i32 @main() { call void @callee() ret i32 0 }
    )");
  AnalysisSession::Options options;
  options.call_graph = lotus::CallGraphAnalysisType::CHA;
  auto session = std::make_shared<AnalysisSession>(*module, options);
  IntegerProblem a, b;
  IFDSSolver<IntegerProblem> reference(a);
  DFISolver<IntegerProblem> dfi(b);
  reference.set_analysis_session(session);
  dfi.set_analysis_session(session);
  compare(*module, reference, dfi);
  auto provider = [](const llvm::CallBase &) {
    return CalleeTargets{{}, false};
  };
  reference.set_callee_provider(provider);
  dfi.set_callee_provider(provider);
  compare(*module, reference, dfi);
  EXPECT_TRUE(
      dfi.get_facts_at_entry(&module->getFunction("callee")->front().front())
          .empty());
  auto other = parse("define i32 @main() { ret i32 0 }");
  EXPECT_THROW(dfi.solve(*other), std::invalid_argument);
  EXPECT_THROW(dfi.get_results(), std::logic_error);
}
} // namespace
} // namespace ifds
