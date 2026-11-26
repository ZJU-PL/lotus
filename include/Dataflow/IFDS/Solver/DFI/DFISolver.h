// Adapted from PhASAR's DFISolver (88366fc69).
// Copyright (c) 2026 Fabian Schiebel and others. MIT; see
// third-party/phasar/LICENSE.txt.
#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"
#include "Dataflow/IFDS/Core/IFDSIDESolverConfig.h"
#include "Dataflow/IFDS/Solver/DFI/DFIExplodedSupergraph.h"
#include "Dataflow/IFDS/Solver/DFI/DFIResults.h"

#include <limits>
#include <unordered_set>

namespace ifds {

/// Independent, experimental IFDS backend using DFI-style graph expansion,
/// SCC scheduling and interval-indexed entry/exit summaries. Transfers must be
/// stable during a solve. It consumes the existing IFDSProblem interface; no
/// IDE values, tabulation path-edge storage, or solver inheritance is involved.
template <typename Problem, typename Config = DFIConfig> class DFIIFDSSolver {
public:
  using Fact = typename Problem::FactType;
  using FactSet = typename Problem::FactSet;
  using Node = typename ExplodedSupergraph<Fact>::Node;
  explicit DFIIFDSSolver(Problem &problem) : m_problem(problem) {}
  ~DFIIFDSSolver() { release_alias(); }
  DFIIFDSSolver(const DFIIFDSSolver &) = delete;
  DFIIFDSSolver &operator=(const DFIIFDSSolver &) = delete;

  void set_analysis_session(std::shared_ptr<AnalysisSession> session) {
    m_problem.set_analysis_session(std::move(session));
  }
  void set_icfg(std::shared_ptr<dataflow::controlflow::InterCFG> icfg) {
    m_icfg = std::move(icfg);
  }
  void set_callee_provider(CalleeProvider provider) {
    m_provider = std::move(provider);
  }
  void set_solver_config(IFDSIDESolverConfig config) { m_config = config; }
  IFDSIDESolverConfig &get_solver_config() { return m_config; }
  const IFDSIDESolverConfig &get_solver_config() const { return m_config; }
  const DFIStatistics &get_statistics() const { return m_statistics; }
  size_t get_sparse_transfers() const { return m_sparse_transfers; }
  size_t get_steps_performed() const { return m_statistics.expanded_vertices; }

  void solve(const llvm::Module &module) {
    m_complete = false;
    m_statistics = {};
    m_sparse_transfers = 0;
    m_data = std::make_shared<dfi::ResultsData<Fact>>();
    m_seen_summaries.clear();
    m_graph.reset();
    m_snapshot.reset();
    auto session = m_problem.analysis_session();
    if (session && &session->module() != &module)
      throw std::invalid_argument("DFI session belongs to a different module");
    release_alias();
    if (m_config.auto_inject_alias_analysis() &&
        !m_problem.has_alias_analysis_configured()) {
      m_owned_alias = std::make_unique<lotus::AliasAnalysisWrapper>(
          const_cast<llvm::Module &>(module), m_config.alias_analysis_config());
      m_problem.set_alias_analysis(m_owned_alias.get());
    }
    if (session && !m_icfg && !m_provider) {
      m_snapshot = session->graph();
      m_functions = session->function_index();
      m_sccs = session->call_graph_sccs();
    } else {
      auto *aa = m_problem.alias_analysis();
      m_snapshot = std::make_shared<AnalysisGraph>(
          const_cast<llvm::Module &>(module), m_icfg, m_provider,
          aa ? lotus::SparseLLVMControlFlow::AliasCheckFn(
                   [aa](const llvm::Value *a, const llvm::Value *b) {
                     return aa->mayAlias(a, b);
                   })
             : lotus::SparseLLVMControlFlow::AliasCheckFn{});
      m_functions = std::make_shared<FunctionIndex>(module);
      m_sccs = std::make_shared<CallGraphSCCs>(m_functions, m_snapshot);
    }
    m_graph = std::make_unique<dfi::ExplodedSupergraph>(
        m_sccs->components(), m_config.follow_returns_past_seeds());
    fact_id(m_problem.zero_fact()); // reserved ID 0
    auto seeds = m_problem.initial_seeds(module);
    for (const auto &seed : seeds.get_seeds()) {
      FactSet facts = seed.second;
      if (m_problem.auto_add_zero())
        facts.insert(m_problem.zero_fact());
      for (const auto &fact : facts)
        m_graph->addSeed(vertex(seed.first, fact));
    }
    while (auto next = m_graph->nextAction()) {
      if (next->expand) {
        auto batch = m_graph->takeWorklist(next->function);
        for (auto v : batch) {
          expand(v);
          ++m_statistics.expanded_vertices;
        }
      } else {
        auto events = m_graph->summarize(next->function);
        ++m_statistics.local_index_builds;
        for (auto event : events.summaries)
          apply_summary(event.call, event.entry, event.exit);
        for (auto exit : events.unbalanced_exits)
          return_unbalanced(exit);
      }
    }
    m_statistics.summary_edges = m_graph->summaryEdgeCount();
    if constexpr (Config::BuildQueryIndex) {
      auto graph = m_graph->buildQueryGraph();
      m_data->direction = Config::IndexDirection;
      if constexpr (Config::IndexDirection == DFIIndexDirection::Backward) {
        lotus::graph::Adjacency reversed(graph.size());
        for (dfi::Id v = 0; v < graph.size(); ++v)
          for (auto to : graph[v])
            reversed[to].push_back(v);
        graph = std::move(reversed);
      }
      m_data->index =
          std::make_shared<lotus::graph::IntervalReachabilityIndex>(graph);
      m_statistics.query_intervals = m_data->index->numIntervals();
    }
    m_graph.reset(); // release mutable solving graph and per-entry summaries
    m_seen_summaries.clear();
    m_complete = true;
  }

  DFIResults<Fact> get_results() const {
    require_complete();
    return DFIResults<Fact>(m_data);
  }
  DFIReachability<Fact> get_reachability() const {
    require_complete();
    return DFIReachability<Fact>(m_data);
  }
  FactSet get_facts_at_entry(const llvm::Instruction *inst) const {
    return get_results().get_facts_at_entry(inst);
  }
  FactSet get_facts_at_exit(const llvm::Instruction *inst) const {
    return get_results().get_facts_at_exit(inst);
  }
  FactSet get_facts_at_in_llvm_ssa(const llvm::Instruction *inst) const {
    return get_results().get_facts_at_in_llvm_ssa(inst);
  }
  bool fact_reaches(const Fact &fact, const llvm::Instruction *inst) const {
    return get_facts_at_exit(inst).count(fact) != 0;
  }
  void get_summary_edges(std::vector<SummaryEdge<Fact>> &out) const {
    get_results().get_summary_edges(out);
  }

private:
  void require_complete() const {
    if (!m_complete)
      throw std::logic_error("DFI has no completed results");
  }
  void release_alias() {
    if (m_owned_alias && m_problem.alias_analysis() == m_owned_alias.get())
      m_problem.set_alias_analysis(nullptr);
    m_owned_alias.reset();
  }
  template <typename T>
  static dfi::Id checked_id(const std::vector<T> &values) {
    if (values.size() >= std::numeric_limits<dfi::Id>::max() - 2)
      throw std::length_error("DFI ID space exhausted");
    return static_cast<dfi::Id>(values.size());
  }
  dfi::Id fact_id(const Fact &fact) {
    auto it = m_data->fact_ids.find(fact);
    if (it != m_data->fact_ids.end())
      return it->second;
    auto id = checked_id(m_data->facts);
    m_data->fact_ids.emplace(fact, id);
    m_data->facts.push_back(fact);
    return id;
  }
  dfi::Id vertex(const llvm::Instruction *inst, const Fact &fact) {
    if (!inst || !inst->getParent() || !inst->getFunction() ||
        inst->getModule() != &m_snapshot->module())
      throw std::invalid_argument("DFI node belongs to a different module");
    auto it = m_data->instruction_ids.find(inst);
    dfi::Id instruction;
    if (it != m_data->instruction_ids.end()) {
      instruction = it->second;
    } else {
      instruction = checked_id(m_data->instructions);
      m_data->instruction_ids.emplace(inst, instruction);
      m_data->instructions.push_back(inst);
    }
    auto function = m_functions->id(inst->getFunction());
    if (!function)
      throw std::invalid_argument("DFI node function is not indexed");
    auto id = m_graph->vertex(instruction, fact_id(fact), function->value,
                              llvm::isa<llvm::ReturnInst>(inst));
    if (id == m_data->nodes.size()) {
      auto fact_id = m_graph->get(id).fact;
      m_data->nodes.push_back({instruction, fact_id});
      m_data->vertex_of.emplace(dfi::node_key(instruction, fact_id), id);
      m_data->at[inst].push_back(id);
    }
    return id;
  }
  FactSet with_zero(FactSet facts, const Fact &source) const {
    if (m_problem.auto_add_zero() && m_problem.is_zero_fact(source))
      facts.insert(m_problem.zero_fact());
    return facts;
  }
  const llvm::Instruction *instruction(dfi::Id v) const {
    return m_data->instructions[m_graph->get(v).instruction];
  }
  Fact fact(dfi::Id v) const { return m_data->facts[m_graph->get(v).fact]; }
  void add_flows(dfi::Id from, const llvm::Instruction *to,
                 const FactSet &facts) {
    for (const auto &target : facts)
      m_graph->addEdge(from, vertex(to, target));
  }
  bool certified_identity(const llvm::Instruction *inst, const Fact &source,
                          const std::vector<const llvm::Instruction *> &succs) {
    if (!m_config.sparse_execution() || llvm::isa<llvm::CallBase>(inst) ||
        inst->isTerminator() || &inst->getParent()->front() == inst ||
        succs.size() != 1 || succs.front() != inst->getNextNode())
      return false;
    auto *value = m_problem.sparse_fact_value(source);
    return value && !m_snapshot->sparse_cfg().shouldKeepInst(inst, value) &&
           m_problem.is_identity_flow(inst, succs.front(), source);
  }
  void expand(dfi::Id v) {
    auto *inst = instruction(v);
    const Fact source = fact(v);
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
      expand_call(v, call, source);
    } else if (!llvm::isa<llvm::ReturnInst>(inst)) {
      const auto &succs = m_snapshot->successors(inst);
      if (certified_identity(inst, source, succs)) {
        add_flows(v, succs.front(), FactSet{source});
        m_data->exits[inst].insert(source);
        ++m_sparse_transfers;
      } else {
        std::unordered_set<const llvm::Instruction *> seen_successors;
        for (auto *succ : succs) {
          if (!seen_successors.insert(succ).second)
            continue;
          auto facts =
              with_zero(m_problem.normal_flow(inst, succ, source), source);
          ++m_statistics.normal_flow_evaluations;
          m_data->exits[inst].insert(facts.begin(), facts.end());
          add_flows(v, succ, facts);
        }
      }
    }
  }
  void expand_call(dfi::Id v, const llvm::CallBase *call, const Fact &source) {
    auto it = m_snapshot->callees().find(call);
    std::vector<const llvm::Function *> callees =
        it == m_snapshot->callees().end()
            ? std::vector<const llvm::Function *>{nullptr}
            : it->second;
    for (auto *site : m_snapshot->return_sites(call)) {
      // Lotus summary_flow is additive, including when a body is available.
      for (auto *callee : callees) {
        auto facts =
            with_zero(m_problem.summary_flow(call, callee, source), source);
        for (const auto &target : facts)
          m_graph->addSummaryEdge(v, vertex(site, target));
      }
      auto facts = with_zero(
          m_problem.call_to_return_flow(call, site, callees, source), source);
      m_data->exits[call].insert(facts.begin(), facts.end());
      add_flows(v, site, facts);
    }
    for (auto *callee : callees) {
      if (!callee || callee->isDeclaration() || callee->empty())
        continue;
      auto entries =
          with_zero(m_problem.call_flow(call, callee, source), source);
      ++m_statistics.call_flow_evaluations;
      for (const auto &entry_fact : entries) {
        auto entry = vertex(&callee->getEntryBlock().front(), entry_fact);
        auto known_exits = m_graph->addCallEdge(v, entry);
        for (auto exit : known_exits)
          apply_summary(v, entry, exit);
      }
    }
  }
  void apply_summary(dfi::Id call_vertex, dfi::Id entry, dfi::Id exit) {
    auto *call = llvm::cast<llvm::CallBase>(instruction(call_vertex));
    auto *callee = m_functions->function({m_graph->get(entry).function});
    const auto exit_fact = fact(exit), call_fact = fact(call_vertex);
    for (auto *site : m_snapshot->return_sites(call)) {
      auto facts =
          with_zero(m_problem.return_flow(call, instruction(exit), site, callee,
                                          exit_fact, call_fact),
                    exit_fact);
      ++m_statistics.return_flow_evaluations;
      for (const auto &target : facts) {
        m_graph->addSummaryEdge(call_vertex, vertex(site, target));
        SummaryEdge<Fact> summary(call, site, call_fact, target);
        if (m_seen_summaries.insert(summary).second)
          m_data->summaries.push_back(std::move(summary));
      }
    }
  }
  void return_unbalanced(dfi::Id exit) {
    auto *callee = m_functions->function({m_graph->get(exit).function});
    auto it = m_snapshot->callers().find(callee);
    if (it == m_snapshot->callers().end())
      return;
    const auto exit_fact = fact(exit);
    for (auto *call : it->second)
      for (auto *site : m_snapshot->return_sites(call)) {
        auto facts =
            m_problem.return_flow(call, instruction(exit), site, callee,
                                  exit_fact, m_problem.zero_fact());
        ++m_statistics.return_flow_evaluations;
        for (const auto &target : facts)
          if (!m_problem.is_zero_fact(target))
            m_graph->addUnbalancedReturn(exit, vertex(site, target));
      }
  }
  Problem &m_problem;
  IFDSIDESolverConfig m_config;
  bool m_complete = false;
  DFIStatistics m_statistics;
  size_t m_sparse_transfers = 0;
  std::unique_ptr<lotus::AliasAnalysisWrapper> m_owned_alias;
  std::shared_ptr<dataflow::controlflow::InterCFG> m_icfg;
  CalleeProvider m_provider;
  std::shared_ptr<const AnalysisGraph> m_snapshot;
  std::shared_ptr<const FunctionIndex> m_functions;
  std::shared_ptr<const CallGraphSCCs> m_sccs;
  std::unique_ptr<dfi::ExplodedSupergraph> m_graph;
  std::shared_ptr<dfi::ResultsData<Fact>> m_data;
  std::unordered_set<SummaryEdge<Fact>, SummaryEdgeHash<Fact>> m_seen_summaries;
};

template <typename Problem, typename Config = DFIConfig>
using DFISolver = DFIIFDSSolver<Problem, Config>;
} // namespace ifds
