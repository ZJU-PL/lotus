/*
 * lotus-dfa-ifds
 *
 * Dataflow testing tool: IFDS engine.
 */

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/IFDS/Analyses/IFDSReachingDefinitions.h"
#include "Dataflow/IFDS/Analyses/IFDSTaintAnalysis.h"
#include "Dataflow/IFDS/Analyses/IFDSUninitializedVariables.h"
#include "Dataflow/IFDS/Solver/IFDSSolver.h"
#include "Dataflow/Tooling/ToolSupport.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<bitcode>"),
                                          cl::Required);
static cl::opt<std::string> OutDir("out-dir", cl::desc("Output directory"),
                                   cl::value_desc("dir"), cl::init(""));
static cl::opt<bool> StdoutOpt(
    "stdout",
    cl::desc("Write analysis results to stdout when --out-dir is not set"),
    cl::init(false));
static cl::opt<std::string> AnalysisOpt(
    "analysis",
    cl::desc("Analysis: reaching_defs (default), uninitialized, taint"),
    cl::init("reaching_defs"));
static cl::opt<bool> SparseOpt("sparse",
                               cl::desc("Skip certified identity transfers"));
static cl::opt<bool>
    StatisticsOpt("statistics",
                  cl::desc("Print solver work and timing to stderr"));
static cl::opt<std::string>
    CallGraphOpt("call-graph",
                 cl::desc("Call graph: noresolve, cha, rta, vta, otf"),
                 cl::init("otf"));
static cl::opt<std::string> TaintModelOpt("taint-config",
                                          cl::desc("Taint specification file"),
                                          cl::init(""));
static cl::opt<bool> TaintMainArgsOpt(
    "taint-main-args",
    cl::desc("Treat pointer parameters of main as taint sources"),
    cl::init(false));
static cl::list<std::string>
    EntryPointsOpt("entry-point",
                   cl::desc("Analysis root (repeatable; default main)"));

namespace {

using lotus::dataflow_tool::FunctionView;
using lotus::dataflow_tool::ValueIdMap;

template <typename Problem>
void solve(ifds::IFDSSolver<Problem> &solver, Module &module,
           const std::shared_ptr<ifds::AnalysisSession> &session) {
  solver.set_analysis_session(session);
  solver.get_solver_config().set_sparse_execution(SparseOpt);
  auto start = std::chrono::steady_clock::now();
  solver.solve(module);
  auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  if (StatisticsOpt)
    errs() << "processed_edges=" << solver.get_steps_performed()
           << " sparse_transfers=" << solver.get_sparse_transfers()
           << " solve_seconds=" << elapsed << "\n";
}

std::string formatIFDSFact(const ifds::DefinitionFact &Fact,
                           const ValueIdMap &ValueToId) {
  if (Fact.is_zero())
    return "zero";
  std::ostringstream ss;
  auto VarIt = ValueToId.find(Fact.get_variable());
  auto DefIt = ValueToId.find(Fact.get_definition_site());
  ss << "def(" << (VarIt != ValueToId.end() ? VarIt->second : "v") << ","
     << (DefIt != ValueToId.end() ? DefIt->second : "i") << ")";
  return ss.str();
}

std::string formatIFDSFact(const ifds::UninitVarFact &Fact,
                           const ValueIdMap &ValueToId) {
  if (Fact.is_zero())
    return "zero";
  std::ostringstream ss;
  auto It = ValueToId.find(Fact.value);
  ss << (Fact.is_uninitialized() ? "uninit(" : "init(")
     << (It != ValueToId.end() ? It->second : "v") << ")";
  return ss.str();
}

template <typename FactT>
void formatIFDSFactSet(raw_ostream &OS, const std::set<FactT> &Facts,
                       const ValueIdMap &ValueToId) {
  std::vector<std::string> formatted;
  for (const auto &Fact : Facts)
    formatted.push_back(formatIFDSFact(Fact, ValueToId));
  std::sort(formatted.begin(), formatted.end());
  for (size_t i = 0; i < formatted.size(); ++i) {
    if (i)
      OS << ",";
    OS << formatted[i];
  }
}

const Instruction *getNextInstruction(const Instruction *I) {
  if (auto *Next = I->getNextNode())
    return Next;
  for (auto *Succ : successors(I->getParent())) {
    if (Succ->isLandingPad() || Succ->empty())
      continue;
    return &Succ->front();
  }
  return nullptr;
}

template <typename Fact, typename ResultsT>
void printIFDSResults(raw_ostream &OS, const FunctionView &View,
                      const ResultsT &AllResults) {
  lotus::dataflow_tool::printInstructionStates(OS, View, [&](Instruction *I) {
    if (const Instruction *NextInst = getNextInstruction(I)) {
      auto Node =
          typename ifds::ExplodedSupergraph<Fact>::Node(NextInst, Fact::zero());
      auto It = AllResults.find(Node);
      if (It != AllResults.end())
        formatIFDSFactSet(OS, It->second, View.ValueToId);
    }
  });
}

void runReachingDefinitions(
    raw_ostream &OS, Module &M,
    const std::shared_ptr<ifds::AnalysisSession> &session) {
  ifds::ReachingDefinitionsAnalysis Problem;
  ifds::IFDSSolver<ifds::ReachingDefinitionsAnalysis> Solver(Problem);
  solve(Solver, M, session);
  const auto AllResults = Solver.get_all_results();
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    auto View = lotus::dataflow_tool::buildFunctionView(F);
    OS << "FUNC " << F.getName() << "\n";
    printIFDSResults<ifds::DefinitionFact>(OS, View, AllResults);
  }
}

void runUninitialized(raw_ostream &OS, Module &M,
                      const std::shared_ptr<ifds::AnalysisSession> &session) {
  ifds::UninitializedVariablesAnalysis Problem;
  ifds::IFDSSolver<ifds::UninitializedVariablesAnalysis> Solver(Problem);
  solve(Solver, M, session);
  const auto AllResults = Solver.get_all_results();
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    auto View = lotus::dataflow_tool::buildFunctionView(F);
    OS << "FUNC " << F.getName() << "\n";
    printIFDSResults<ifds::UninitVarFact>(OS, View, AllResults);
  }
}

void runTaint(raw_ostream &OS, Module &M,
              const std::shared_ptr<ifds::AnalysisSession> &session) {
  TaintConfig empty;
  ifds::TaintAnalysis::Config config;
  config.taint_main_args = TaintMainArgsOpt;
  ifds::TaintAnalysis Problem(config, empty);
  ifds::IFDSSolver<ifds::TaintAnalysis> Solver(Problem);
  solve(Solver, M, session);
  Problem.report_vulnerabilities(Solver, OS);
}

struct AnalysisHandler final {
  StringRef Name;
  void (*Run)(raw_ostream &, Module &,
              const std::shared_ptr<ifds::AnalysisSession> &);
};

const AnalysisHandler Handlers[] = {
    {"reaching_defs", &runReachingDefinitions},
    {"uninitialized", &runUninitialized},
    {"taint", &runTaint},
};

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "IFDS engine testing\n");

  LLVMContext Context;
  SMDiagnostic Err;
  auto M = lotus::dataflow_tool::loadModuleOrReport(InputFilename, Context, Err,
                                                    argv[0]);
  if (!M)
    return 1;

  lotus::dataflow_tool::prepareModule(*M);

  ifds::AnalysisSession::Options options;
  options.call_graph = lotus::toCallGraphAnalysisType(CallGraphOpt);
  if (options.call_graph == lotus::CallGraphAnalysisType::Invalid) {
    errs() << "error: unknown call graph analysis '" << CallGraphOpt << "'\n";
    return 1;
  }
  if (!EntryPointsOpt.empty())
    options.entry_points.assign(EntryPointsOpt.begin(), EntryPointsOpt.end());
  std::unique_ptr<TaintConfig> model;
  if (!TaintModelOpt.empty())
    model = TaintConfigParser::parse_file(TaintModelOpt);
  else if (AnalysisOpt == "taint")
    model = TaintConfigParser::parse_default();
  if ((!TaintModelOpt.empty() || AnalysisOpt == "taint") && !model) {
    errs() << "error: could not load taint model\n";
    return 1;
  }
  std::shared_ptr<ifds::AnalysisSession> session;
  try {
    session = std::make_shared<ifds::AnalysisSession>(*M, options, model.get());
  } catch (const std::invalid_argument &error) {
    errs() << "error: " << error.what() << "\n";
    return 1;
  }

  raw_null_ostream NullOS;
  std::unique_ptr<raw_fd_ostream> FileOS;
  std::error_code EC;
  raw_ostream &OS = lotus::dataflow_tool::selectOutputStream(
      StdoutOpt, OutDir, "ifds.txt", FileOS, NullOS, EC);
  if (EC) {
    errs() << "error: cannot create " << OutDir << "/ifds.txt: " << EC.message()
           << "\n";
    return 1;
  }

  const auto *Handler =
      lotus::dataflow_tool::findHandler(AnalysisOpt, Handlers);
  if (!Handler) {
    errs() << "error: unknown IFDS analysis '" << AnalysisOpt << "'\n";
    return 1;
  }

  OS << "[ifds:" << AnalysisOpt << "]\n";
  Handler->Run(OS, *M, session);
  return 0;
}
