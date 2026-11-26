#include "Dataflow/IFDS/Core/SharedAnalysisServices.h"

#include "Dataflow/IFDS/Core/AnalysisSession.h"

#include <limits>
#include <stdexcept>
#include <unordered_set>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>

namespace ifds {
FunctionIndex::FunctionIndex(const llvm::Module &module) : m_module(module) {
  if (module.size() > std::numeric_limits<uint32_t>::max() - 2)
    throw std::length_error("Too many functions");
  for (const auto &function : module) {
    FunctionId id{static_cast<uint32_t>(m_functions.size())};
    m_functions.push_back(&function);
    m_ids.emplace(&function, id);
  }
}
std::optional<FunctionId>
FunctionIndex::id(const llvm::Function *function) const {
  auto it = m_ids.find(function);
  return it == m_ids.end() ? std::nullopt
                           : std::optional<FunctionId>(it->second);
}
const llvm::Function *FunctionIndex::function(FunctionId id) const {
  return id.value < size() ? m_functions[id.value] : nullptr;
}
CallGraphSCCs::CallGraphSCCs(std::shared_ptr<const FunctionIndex> functions,
                             std::shared_ptr<const AnalysisGraph> graph)
    : m_functions(std::move(functions)), m_graph(std::move(graph)) {
  if (!m_functions || !m_graph || &m_functions->module() != &m_graph->module())
    throw std::invalid_argument("Function index and graph must share a module");
  lotus::graph::Adjacency edges(m_functions->size());
  for (const auto &entry : m_graph->callees()) {
    auto from = m_functions->id(entry.first->getFunction());
    if (!from)
      throw std::invalid_argument("Call site missing from function index");
    for (auto *callee : entry.second)
      if (callee) {
        auto to = m_functions->id(callee);
        if (!to)
          throw std::invalid_argument("Callee missing from function index");
        edges[from->value].push_back(to->value);
      }
  }
  // Ignore hash-map iteration order when assigning SCC IDs.
  for (auto &row : edges) {
    std::sort(row.begin(), row.end());
    row.erase(std::unique(row.begin(), row.end()), row.end());
  }
  m_sccs = std::make_shared<lotus::graph::SCCGraph>(edges);
}
namespace {
void collectGlobals(const llvm::Value *root, GlobalUsageSummary &summary,
                    std::unordered_set<const llvm::Value *> &seen) {
  std::vector<const llvm::Value *> work{root};
  while (!work.empty()) {
    auto *value = work.back();
    work.pop_back();
    if (!seen.insert(value).second)
      continue;
    if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(value)) {
      summary.referenced.insert(global);
      if (global->hasInitializer())
        work.push_back(global->getInitializer());
    } else if (auto *alias = llvm::dyn_cast<llvm::GlobalAlias>(value)) {
      work.push_back(alias->getAliasee());
    } else if (auto *constant = llvm::dyn_cast<llvm::Constant>(value)) {
      // Functions/block addresses are identities, not containers of references.
      if (llvm::isa<llvm::GlobalValue>(constant) ||
          llvm::isa<llvm::BlockAddress>(constant))
        continue;
      for (const auto &operand : constant->operands())
        work.push_back(operand.get());
    }
  }
}
void merge(GlobalUsageSummary &into, const GlobalUsageSummary &from) {
  into.referenced.insert(from.referenced.begin(), from.referenced.end());
  into.may_access_unknown_globals |= from.may_access_unknown_globals;
}
} // namespace
UsedGlobals::UsedGlobals(std::shared_ptr<const CallGraphSCCs> sccs)
    : m_sccs(std::move(sccs)) {
  if (!m_sccs)
    throw std::invalid_argument("Missing call graph SCCs");
  auto functions = m_sccs->functions();
  auto graph = m_sccs->graph();
  auto components = m_sccs->components();
  m_direct.resize(functions->size());
  m_transitive.resize(components->size());
  for (uint32_t id = 0; id < functions->size(); ++id) {
    auto *function = functions->function({id});
    auto &summary = m_direct[id];
    if (function->isDeclaration()) {
      summary.may_access_unknown_globals = !function->doesNotAccessMemory();
      continue;
    }
    std::unordered_set<const llvm::Value *> seen;
    for (const auto &inst : llvm::instructions(function)) {
      if (llvm::isa<llvm::DbgInfoIntrinsic>(inst))
        continue;
      for (const auto &operand : inst.operands())
        collectGlobals(operand.get(), summary, seen);
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        // Intrinsics can also have memory effects through pointer arguments;
        // those are handled below, in addition to their callee summary.
        auto targets = graph->callees().find(call);
        if (!call->doesNotAccessMemory() &&
            (targets == graph->callees().end() ||
             std::find(targets->second.begin(), targets->second.end(),
                       nullptr) != targets->second.end()))
          summary.may_access_unknown_globals = true;
      }
      if (inst.mayReadOrWriteMemory()) {
        // A parameter or loaded pointer may refer to an unnamed global. Direct
        // globals and local alloca storage are identifiable without AA.
        // Scanning all pointer operands is intentionally conservative (e.g.
        // pointer stores).
        for (const auto &operand : inst.operands()) {
          auto *value = operand.get();
          if (!value->getType()->isPointerTy() ||
              llvm::isa<llvm::Function>(value))
            continue;
          auto *base = llvm::getUnderlyingObject(value);
          if (!llvm::isa<llvm::GlobalVariable>(base) &&
              !llvm::isa<llvm::AllocaInst>(base) &&
              !llvm::isa<llvm::ConstantPointerNull>(base))
            summary.may_access_unknown_globals = true;
        }
      }
    }
  }
  for (uint32_t c = 0; c < components->size(); ++c) {
    auto &summary = m_transitive[c];
    for (auto id : components->members(c))
      merge(summary, m_direct[id]);
    for (auto callee : components->successors(c))
      merge(summary, m_transitive[callee]);
  }
}
const GlobalUsageSummary &UsedGlobals::direct(FunctionId function) const {
  return m_direct.at(function.value);
}
const GlobalUsageSummary &UsedGlobals::transitive(FunctionId function) const {
  return component(m_sccs->componentOf(function));
}
const GlobalUsageSummary &UsedGlobals::component(uint32_t scc) const {
  return m_transitive.at(scc);
}
} // namespace ifds
