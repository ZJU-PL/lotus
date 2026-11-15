// SPDX-License-Identifier: MIT
#include "CFL/Classical/Solvers/Engines/SparseSubset/SparseSubsetCFL.h"
#include "CFL/Classical/Solvers/Engines/SparseSubset/SparseSubsetTabulation.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <stdexcept>
#include <unordered_set>

namespace lotus::cfl::classical::engines::sparse_subset {
namespace {
struct Work {
  SymbolId symbol;
  NodeId source, target;
};
struct RuleInstances {
  BinaryRuleId rule;
  TwoSidedSetPair row, column;
  RuleInstances(BinaryRuleId r, std::size_t n,
                TwoSidedSetPair::Options o)
      : rule(r), row(n,o), column(n,o) {}
};
} // namespace

std::unique_ptr<Relation> solveAllPairs(const LabeledGraph &graph,
                                        const Grammar &grammar,
                                        double epsilon) {
  const auto issues = grammar.validate();
  for (const auto &issue : issues) {
    if (issue.severity == GrammarIssueSeverity::Error)
      throw std::invalid_argument("invalid grammar: " + issue.message);
  }
  if (!(epsilon > 0.0 && epsilon < 1.0))
    throw std::invalid_argument("sparse-subset epsilon must be in (0,1)");
  const auto n = graph.vertexCount();
  auto relation = createRelation(RelationBackend::SparseSets, n);
  if (n == 0) return relation;
  std::deque<Work> work;

  // Parameters follow the paper's balancing: D ~= n/log^2 n, g ~= log n,
  // s ~= (eps/4)log n/log(e*n/D), L=s*ceil(n/D). For small n, clamp to
  // bounded values while preserving the block/group alignment invariant.
  const std::size_t lambda = std::max<std::size_t>(1,
      static_cast<std::size_t>(std::ceil(std::log2(std::max<std::size_t>(2,n)))));
  const std::size_t D = std::max<std::size_t>(1,(n+lambda*lambda-1)/(lambda*lambda));
  const std::size_t g = lambda;
  // The paper fixes eta=epsilon/4; s is the largest integer satisfying
  // the sparse-subset table's n^eta size budget.
  const double eta = epsilon/4.0;
  const auto ratio = std::max<std::size_t>(1,(n+D-1)/D);
  const auto denominator = std::max<double>(1.0,
      std::log2(std::exp(1.0)*static_cast<double>(ratio)));
  const std::size_t s = std::max<std::size_t>(1,
      static_cast<std::size_t>(std::floor(eta*std::log2(std::max<std::size_t>(2,n))/denominator)));
  const std::size_t L = std::max<std::size_t>(1,s*ratio);
  TwoSidedSetPair::Options options;
  options.groups = g;
  options.promotion_threshold = D;
  options.block_size = L;
  options.sparsity = s;

  std::vector<RuleInstances> instances;
  std::size_t binary_rule_count=0;
  for(const auto &entry:grammar.binaryByFirstId()) binary_rule_count+=entry.second.size();
  instances.reserve(binary_rule_count);
  for(const auto &entry:grammar.binaryByFirstId())
    for(const auto &rule:entry.second) instances.emplace_back(rule,n,options);
  std::vector<std::vector<std::size_t>> row_trigger(grammar.symbolCount()),
      column_trigger(grammar.symbolCount());
  struct Notification { std::size_t instance; bool row; bool x_family; };
  std::vector<std::vector<Notification>> notifications(grammar.symbolCount());
  for(std::size_t i=0;i<instances.size();++i) {
    const auto &r=instances[i].rule;
    row_trigger[r.first].push_back(i);
    column_trigger[r.second].push_back(i);
    notifications[r.second].push_back({i,true,true});
    notifications[r.lhs].push_back({i,true,false});
    notifications[r.first].push_back({i,false,true});
    notifications[r.lhs].push_back({i,false,false});
  }
  auto notify=[&](const Work &e) {
    for(const auto &nfo:notifications[e.symbol]) {
      auto &instance=instances[nfo.instance];
      if(nfo.row) {
        if(nfo.x_family) instance.row.insertX(e.source,e.target);
        else instance.row.insertY(e.source,e.target);
      } else {
        if(nfo.x_family) instance.column.insertX(e.target,e.source);
        else instance.column.insertY(e.target,e.source);
      }
    }
  };
  // Notify all row/column instances at insertion time, before the fact is
  // queued. This preserves the paper's invariant that every completed report
  // sees all previously inserted facts in its Y family.
  auto insert = [&](SymbolId symbol, NodeId source, NodeId target) {
    if (!relation->add(symbol, source, target)) return false;
    Work fact{symbol, source, target};
    notify(fact);
    work.push_back(fact);
    return true;
  };
  // Seed terminals before nullable/unit propagation.
  for(const auto &edge:graph.edges()) {
    if(!grammar.isTerminal(edge.label))
      throw std::invalid_argument("input graph edge label is not a grammar terminal");
    insert(grammar.symbolId(edge.label),edge.source,edge.target);
  }
  for(SymbolId s0:grammar.nullableSymbolIds())
    for(NodeId v=0;v<n;++v) insert(s0,v,v);

  while(!work.empty()) {
    const Work e=work.front(); work.pop_front();
    if(auto it=grammar.unaryByRhsId().find(e.symbol);
       it!=grammar.unaryByRhsId().end())
      for(SymbolId lhs:it->second) insert(lhs,e.source,e.target);

    for(auto id:row_trigger[e.symbol]) {
      auto &inst=instances[id];
      // B(u,v): report R_C[v,:] \\ R_A[u,:].
      for(NodeId w:inst.row.report(e.target,e.source))
        insert(inst.rule.lhs,e.source,w);
    }
    for(auto id:column_trigger[e.symbol]) {
      auto &inst=instances[id];
      // C(v,x): report R_B[:,v] \\ R_A[:,x].
      for(NodeId u:inst.column.report(e.source,e.target))
        insert(inst.rule.lhs,u,e.target);
    }
  }
  return relation;
}
} // namespace lotus::cfl::classical::engines::sparse_subset
