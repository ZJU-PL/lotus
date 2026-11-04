#include "Checker/GSAF/Engine/Checker.h"

#include "Checker/GSAF/Engine/FunctionAnalyzer.h"
#include "Checker/GSAF/Report/ReportDecorator.h"
#include "Utils/ADT/PushPopCache.h"
#include "Utils/Parallel/Scheduler/PipelineScheduler.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus::gsaf {

namespace {
llvm::Error
writeGraphStatistics(llvm::Module &module,
                     gvfg::GuardedValueFlowGraphBuilderPass &builder,
                     llvm::StringRef filename) {
  llvm::json::Array functions;
  for (auto &function : module) {
    if (!builder.hasGraphFor(function))
      continue;
    const auto &graph = builder.getGraph(function);
    uint64_t parents = 0;
    for (const auto &node : graph.nodes())
      parents += node->parents().size();
    functions.push_back(llvm::json::Object{
        {"FunctionName", function.getName()},
        {"NodeBaseNum", static_cast<int64_t>(graph.nodes().size())},
        {"ParentNodeNum", static_cast<int64_t>(parents)}});
  }
  std::error_code error;
  llvm::raw_fd_ostream output(filename, error, llvm::sys::fs::OF_None);
  if (error)
    return llvm::errorCodeToError(error);
  output << llvm::json::Value(
      llvm::json::Object{{"GVFGNodeData", std::move(functions)}});
  output.flush();
  if (output.has_error())
    return llvm::errorCodeToError(output.error());
  return llvm::Error::success();
}
} // namespace

GSAFSummary::~GSAFSummary() {
  for (auto &entry : InSmrys)
    for (auto *summary : entry.second)
      delete summary;
  for (auto &entry : OutSmrys)
    for (auto *summary : entry.second)
      delete summary;
  for (auto &entry : RetSmrys)
    delete entry.second;
  for (auto &entry : TaintSourceWrapperSmrys)
    for (auto *summary : entry.second)
      delete summary;
}

char GSAFChecker::ID = 0;

GSAFChecker::GSAFChecker(std::shared_ptr<VulnerabilityWrapper> vulnerabilities)
    : ModulePass(ID), Vuln(std::move(vulnerabilities)),
      ReportTasks(ThreadPool::get()->makeTaskGroup()) {}

GSAFChecker::~GSAFChecker() {
  for (auto &entry : FuncSmryMap)
    delete entry.second;
}

void GSAFChecker::getAnalysisUsage(AnalysisUsage &usage) const {
  usage.setPreservesAll();
  usage.addRequired<gvfg::GuardedValueFlowGraphBuilderPass>();
  usage.addRequired<gvfg::LotusAAWrapper>();
  usage.addRequired<GSAFModels>();
  usage.addRequired<gsa::ControlDependenceAnalysisPass>();
  Vuln->getAnalysisUsage(usage);
}

bool GSAFChecker::runOnModule(llvm::Module &module) {
  if (GSAFOptions::InlineDepth == UINT_MAX)
    GSAFOptions::InlineDepth = 6;
  if (Vuln->size() != 1 ||
      Vuln->getVulnerability(1)->getCategoryType() != Vulnerability::VCT_Taint)
    report_fatal_error(
        "GSAF requires one taint-style vulnerability per analysis pass");
  Module = &module;
  GraphBuilder = &getAnalysis<gvfg::GuardedValueFlowGraphBuilderPass>();
  DL = std::make_unique<PackedTypeLayout>(module.getDataLayout());
  Models = &getAnalysis<GSAFModels>();
  for (auto &function : module)
    DebugInfo.collectMetadata(&function);
  Renderer =
      std::make_unique<ir_expression::IRExpressionRenderer>(*DL, DebugInfo);
  Vuln->initializeAnalysis(this);

  for (auto &entry : FuncSmryMap)
    delete entry.second;
  FuncSmryMap.clear();
  FuncTraceMap.clear();
  Functions.clear();
  for (Function &function : module) {
    if (!GraphBuilder->hasGraphFor(function))
      continue;
    Functions[&function] = std::make_unique<FunctionInfo>(function);
    auto &graph = GraphBuilder->getGraph(function);
    graph.arguments();
    graph.returns();
    FuncSmryMap[&function] = new GSAFSummary;
    FuncTraceMap[&function];
  }

  // Enrich the standard LLVM call graph with the native GVFG's resolved
  // indirect targets before handing dependencies to Lotus's scheduler.
  llvm::CallGraph call_graph(module);
  for (const auto &entry : Functions) {
    auto &graph = GraphBuilder->getGraph(*const_cast<Function *>(entry.first));
    for (const auto &site : graph.sites()) {
      auto *call_site = dyn_cast<gvfg::GuardedValueFlowCallSite>(site.get());
      if (!call_site)
        continue;
      auto *call = dyn_cast<CallBase>(call_site->getInstruction());
      if (!call)
        continue;
      for (Function *callee : call_site->getCallees()) {
        auto target = Functions.find(callee);
        if (target != Functions.end() && !call_site->isBackEdge(callee))
          target->second->HasNonBackEdgeCaller = true;
        if (!call->getCalledFunction())
          call_graph[const_cast<Function *>(entry.first)]->addCalledFunction(
              call, call_graph[callee]);
      }
    }
  }
  PipelineScheduler scheduler(module, call_graph,
                              PipelineScheduler::AT_BottomUp);
  scheduler.setEnableGC(false);
  scheduler.setTaskCallback([this](const Function *function) {
    if (!Functions.count(function))
      return;
    auto worker = std::make_unique<FunctionAnalyzer>(
        this, const_cast<Function *>(function));
    try {
      worker->run();
    } catch (const std::exception &error) {
      worker->handleException();
      errs() << "GSAF: " << function->getName() << ": " << error.what() << '\n';
    }
  });
  scheduler.run();
  ReportTasks.wait();
  if (!GSAFOptions::CollectGVFGData.empty())
    if (auto error = writeGraphStatistics(module, *GraphBuilder,
                                          GSAFOptions::CollectGVFGData))
      logAllUnhandledErrors(std::move(error), errs(), "GSAF statistics: ");
  LLVMValueReportDecorator::inst_resolver = Renderer.get();
  LLVMValueReportDecorator::CDGs =
      &getAnalysis<gsa::ControlDependenceAnalysisPass>();
  LLVMValueReportDecorator::memory_spec = &Models->api();
  auto &manager = BugReportMgr::get_instance();
  std::map<int, std::vector<std::shared_ptr<VulnerabilityTrace>>> tracesByMask;
  for (const auto &entry : FuncTraceMap)
    for (auto item : entry.second.snapshot()) {
      int mask = Vuln->checkTrace(item.second, item.first);
      for (int bit = mask & -mask; mask; mask -= bit, bit = mask & -mask)
        if (!Vuln->isParasitical(bit))
          tracesByMask[bit].push_back(item.second);
    }
  for (auto &entry : tracesByMask) {
    for (auto &trace : entry.second) {
      trace->set_bug_type_importance(100);
      trace->set_score(100);
      trace->set_trace_type(lotus::trace::TraceType::SOURCE_SINK);
      auto *report = Vuln->buildReport(trace, &DebugInfo, entry.first);
      report->set_conf_score(trace->get_score());
      report->set_dominated(false);
      report->set_valid(true);
      manager.insert_report(report->get_bug_type_id(), report, true);
    }
  }
  LLVMValueReportDecorator::inst_resolver = nullptr;
  LLVMValueReportDecorator::CDGs = nullptr;
  LLVMValueReportDecorator::memory_spec = nullptr;
  reporting::DiagnosticBuilder::DIA = nullptr;
  return false;
}

bool GSAFChecker::hasNonBackEdgeCaller(const Function *function) const {
  auto found = Functions.find(function);
  return found != Functions.end() && found->second->HasNonBackEdgeCaller;
}

std::vector<std::shared_ptr<VulnerabilityTrace>>
GSAFChecker::traces(const std::shared_ptr<Vulnerability> &vulnerability) const {
  std::vector<std::shared_ptr<VulnerabilityTrace>> result;
  int mask = Vuln->getMask(vulnerability);
  for (const auto &entry : FuncTraceMap)
    for (const auto &trace : entry.second.snapshot())
      if (trace.first & mask)
        result.push_back(trace.second);
  return result;
}

bool SMTAdvisor4Inlining::prune(const SMTExpr &expression) {
  std::string symbol = expression.getSymbol();
  unsigned depth = 0;
  size_t position = 0;
  while ((position = symbol.find("_CS", position)) != std::string::npos) {
    ++position;
    if (++depth >= GSAFOptions::InlineDepth)
      return true;
  }
  return false;
}

bool SMTAdvisor4Inlining::rename(const SMTExpr &expression) {
  return !StringRef(expression.getSymbol()).startswith("global_");
}

#undef DEBUG_TYPE
#define DEBUG_TYPE "gsaf-composite-checker"

namespace {
class MultiKeyMap {
private:
  typedef std::map<
      std::pair<const GuardedValueFlowObject *, const GuardedValueFlowObject *>,
      std::set<std::shared_ptr<VulnerabilityTrace>>>
      MapTy;
  MapTy HeadTailTraceMap;

public:
  void insert(const GuardedValueFlowObject *Head,
              const GuardedValueFlowObject *Tail,
              std::shared_ptr<VulnerabilityTrace> Trace) {
    assert(Head || Tail);
    if (Head)
      HeadTailTraceMap[std::make_pair(Head, nullptr)].insert(Trace);

    if (Tail)
      HeadTailTraceMap[std::make_pair(nullptr, Tail)].insert(Trace);

    if (Head && Tail)
      HeadTailTraceMap[std::make_pair(Head, Tail)].insert(Trace);

    HeadTailTraceMap[std::make_pair(nullptr, nullptr)].insert(Trace);
  }

  MapTy::iterator
  find(std::pair<const GuardedValueFlowObject *, const GuardedValueFlowObject *>
           Key) {
    return HeadTailTraceMap.find(Key);
  }

  MapTy::iterator end() { return HeadTailTraceMap.end(); }
};

class Builder {
private:
  std::shared_ptr<MultiVulnerability> MultiVuln;

  // TODO this should be a table with multiple indices
  std::map<std::shared_ptr<Vulnerability>, MultiKeyMap> VulnTraceMap;

  PushPopVector<std::shared_ptr<VulnerabilityTrace>> TraceStack;
  PushPopCache<std::shared_ptr<Vulnerability>> VulnCache;

  std::map<
      std::shared_ptr<Vulnerability>,
      std::pair<std::shared_ptr<Vulnerability>, const GuardedValueFlowObject *>>
      HeadMap;
  std::map<
      std::shared_ptr<Vulnerability>,
      std::pair<std::shared_ptr<Vulnerability>, const GuardedValueFlowObject *>>
      TailMap;

public:
  Builder(std::shared_ptr<MultiVulnerability>,
          const std::map<std::shared_ptr<Vulnerability>, GSAFChecker *> &);

  void buildMultiVulnerability();

private:
  void build(size_t I);
};

Builder::Builder(
    std::shared_ptr<MultiVulnerability> MV,
    const std::map<std::shared_ptr<Vulnerability>, GSAFChecker *> &checkers)
    : MultiVuln(MV) {

  // Rearrange traces obtained from checkers for this multi-vulnerability
  LLVM_DEBUG(dbgs() << "\nRearraging subtraces for " << MultiVuln->getFullName()
                    << "\n");
  for (auto VulnIt = MultiVuln->begin(), VulnE = MultiVuln->end();
       VulnIt != VulnE; VulnIt++) {
    auto Vuln = *VulnIt;
    auto *Checker = checkers.at(Vuln);
    assert(Checker);

    LLVM_DEBUG(dbgs() << "\tCollecting traces from part " << Vuln->getName()
                      << " ");
    unsigned TraceCounter = 0;
    for (auto Trace : Checker->traces(Vuln)) {
      auto *Head = Trace->head();
      auto *Tail = Trace->tail();
      VulnTraceMap[Vuln].insert(Head, Tail, Trace);
      TraceCounter++;
    }
    LLVM_DEBUG(dbgs() << "# " << TraceCounter << "\n");

    if (TraceCounter == 0) {
      // If some part cannot be found, just stop
      // building this multi-vulnerability
      VulnTraceMap.clear();
      break;
    }
  }
}

void Builder::buildMultiVulnerability() {
  LLVM_DEBUG(dbgs() << "Building " << MultiVuln->getFullName() << "...\n");

  if (VulnTraceMap.empty()) {
    LLVM_DEBUG(dbgs() << "Done!\n");
    return;
  }

  build(0);

  LLVM_DEBUG(dbgs() << "Done!\n");
}

void Builder::build(size_t Index) {
  std::shared_ptr<Vulnerability> CurrV = MultiVuln->get(Index);

  std::pair<const GuardedValueFlowObject *, const GuardedValueFlowObject *>
      HeadTail = {nullptr, nullptr};
  auto HIt = HeadMap.find(CurrV);
  if (HIt != HeadMap.end() && VulnCache.contains(HIt->second.first)) {
    HeadTail.first = HIt->second.second;
  }
  auto TIt = TailMap.find(CurrV);
  if (TIt != TailMap.end() && VulnCache.contains(TIt->second.first)) {
    HeadTail.second = TIt->second.second;
  }

  auto It = VulnTraceMap[CurrV].find(HeadTail);
  if (It != VulnTraceMap[CurrV].end() && !It->second.empty()) {
    for (auto CurrT : It->second) {
      TraceStack.push();
      VulnCache.push();
      TraceStack.add(CurrT);
      VulnCache.add(CurrV);

      // building head/tail constraints
      for (auto DIt = MultiVuln->head_dep_begin(CurrV),
                DE = MultiVuln->head_dep_end(CurrV);
           DIt != DE; DIt++) {
        auto DepV = *DIt;
        HeadMap[DepV] =
            std::make_pair(CurrV, MultiVuln->getTraceHead(CurrV, CurrT, DepV));
      }
      for (auto DIt = MultiVuln->tail_dep_begin(CurrV),
                DE = MultiVuln->tail_dep_end(CurrV);
           DIt != DE; DIt++) {
        auto DepV = *DIt;
        TailMap[DepV] =
            std::make_pair(CurrV, MultiVuln->getTraceTail(CurrV, CurrT, DepV));
      }

      // TODO In the future, constraints should be checked here

      if (Index + 1 < MultiVuln->size()) {
        build(Index + 1);
      } else {
        // report Trace
        errs() << "\nFind a multi-vulnerability: " << MultiVuln->getFullName()
               << "\n";
        for (auto V : VulnCache.getCacheSet()) {
          errs() << V->getName() << "\n";
        }

        for (auto T : TraceStack.getCacheVector()) {
          errs() << *(T.get()) << "\n";
        }
      }
      VulnCache.pop();
      TraceStack.pop();
    }
  }
}
} // namespace
void buildMultiVulnerability(
    const std::shared_ptr<MultiVulnerability> &vulnerability,
    const std::map<std::shared_ptr<Vulnerability>, GSAFChecker *> &checkers) {
  Builder builder(vulnerability, checkers);
  builder.buildMultiVulnerability();
}

} // namespace lotus::gsaf
