#include "Dataflow/IFDS/Core/AnalysisSession.h"

#include "Analysis/CFG/ExternCallbackModel.h"
#include "Analysis/CFG/GlobalCtorsDtorsModel.h"
#include "Analysis/CallGraph/CallGraphBuilder.h"
#include "Analysis/TypeHierarchy/DIBasedTypeHierarchy.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"
#include "Analysis/CallGraph/OTF/OTFResolver.h"

#include <stdexcept>
#include <unordered_set>

namespace ifds {
namespace {
AnalysisSession::Options prepare(llvm::Module &module,
                                 AnalysisSession::Options options) {
  if (options.call_graph == lotus::CallGraphAnalysisType::Invalid)
    throw std::invalid_argument("Invalid call graph analysis");
  for (const auto &name : options.entry_points) {
    auto *function = module.getFunction(name);
    if (!function || function->isDeclaration() || function->empty())
      throw std::invalid_argument("Missing entry point: " + name);
  }
  if (options.model_external_callbacks)
    lotus::ExternCallbackModel::rewriteCalls(module);
  if (options.model_global_initializers) {
    auto *root =
        lotus::GlobalCtorsDtorsModel::buildModel(module, options.entry_points);
    if (root)
      options.entry_points = {root->getName().str()};
  }
  return options;
}
} // namespace
AnalysisGraph::AnalysisGraph(
    llvm::Module &module, std::shared_ptr<dataflow::controlflow::InterCFG> icfg,
    CalleeProvider provider,
    lotus::SparseLLVMControlFlow::AliasCheckFn may_alias)
    : m_module(module), m_icfg(std::move(icfg)),
      m_sparse(nullptr, std::move(may_alias)) {
  const bool injected_icfg = m_icfg != nullptr;
  if (!m_icfg)
    m_icfg = std::make_shared<dataflow::controlflow::LLVMInterCFG>(&module);
  if (m_icfg->getModule() != &module)
    throw std::invalid_argument("ICFG belongs to a different module");
  for (const auto &function : module) {
    for (const auto &block : function) {
      for (const auto &inst : block) {
        auto *mutable_inst = const_cast<llvm::Instruction *>(&inst);
        for (auto *succ : m_icfg->getSuccsOf(
                 mutable_inst, dataflow::controlflow::FlowDirection::Forward))
          if (succ)
            m_successors[&inst].push_back(succ);
        auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (!call)
          continue;
        if (injected_icfg) {
          for (auto *site : m_icfg->getReturnSitesOfCallAt(mutable_inst))
            if (site)
              m_return_sites[call].push_back(site);
        } else {
          // IFDS/IDE distinguish normal and exceptional continuations.
          m_return_sites[call] = m_successors[&inst];
        }
        CalleeTargets targets;
        if (provider) {
          targets = provider(*call);
        } else {
          for (auto *callee : m_icfg->getCalleesOfCallAt(mutable_inst))
            targets.functions.push_back(callee);
          targets.complete = llvm::isa<llvm::Function>(
              call->getCalledOperand()->stripPointerCastsAndAliases());
        }
        auto &callees = m_callees[call];
        std::unordered_set<const llvm::Function *> seen;
        for (const auto *callee : targets.functions) {
          if (!callee) {
            targets.complete = false;
            continue;
          }
          if (callee->getParent() != &module)
            throw std::invalid_argument("Callee belongs to a different module");
          if (seen.insert(callee).second) {
            callees.push_back(callee);
            m_callers[callee].push_back(call);
          }
        }
        if (!targets.complete || callees.empty())
          callees.push_back(nullptr);
      }
    }
  }
}

const AnalysisGraph::Instructions &
AnalysisGraph::successors(const llvm::Instruction *inst) const {
  static const Instructions empty;
  auto it = m_successors.find(inst);
  return it == m_successors.end() ? empty : it->second;
}
const AnalysisGraph::Instructions &
AnalysisGraph::return_sites(const llvm::CallBase *call) const {
  static const Instructions empty;
  auto it = m_return_sites.find(call);
  return it == m_return_sites.end() ? empty : it->second;
}

AnalysisSession::AnalysisSession(llvm::Module &module)
    : AnalysisSession(module, Options{}) {}
AnalysisSession::AnalysisSession(llvm::Module &module, Options options,
                                 const TaintConfig *taint)
    : m_module(module), m_options(prepare(module, std::move(options))),
      m_taint(taint ? std::make_shared<const TaintConfig>(*taint) : nullptr) {}
AnalysisSession::~AnalysisSession() = default;

std::shared_ptr<lotus::AliasAnalysisWrapper> AnalysisSession::alias_analysis() {
  if (!m_alias)
    m_alias = std::make_shared<lotus::AliasAnalysisWrapper>(
        m_module, m_options.alias_config);
  return m_alias;
}
std::shared_ptr<const lotus::DIBasedTypeHierarchy>
AnalysisSession::type_hierarchy() {
  if (!m_hierarchy)
    m_hierarchy = std::make_shared<lotus::DIBasedTypeHierarchy>(m_module);
  return m_hierarchy;
}
std::shared_ptr<const AnalysisGraph> AnalysisSession::graph() {
  if (m_graph)
    return m_graph;
  type_hierarchy();
  m_vtables = std::make_shared<lotus::LLVMVFTableProvider>(m_module);
  // All bodies get graph data, including those used by custom seeds.
  std::vector<const llvm::Function *> roots;
  for (const auto &function : m_module)
    if (!function.isDeclaration())
      roots.push_back(&function);
  std::unique_ptr<lotus::Resolver> resolver;
  if (m_options.call_graph == lotus::CallGraphAnalysisType::OTF) {
    auto aliases = alias_analysis();
    resolver = std::make_unique<lotus::OTFResolver>(
        &m_module, m_vtables.get(), m_hierarchy.get(),
        [aliases](const llvm::Value *value, const llvm::Instruction *) {
          std::vector<const llvm::Value *> result;
          aliases->getAliasSet(value, result);
          return result;
        });
  } else {
    resolver = lotus::Resolver::create(m_options.call_graph, &m_module,
                                       m_vtables.get(), m_hierarchy.get());
  }
  auto cg = std::make_shared<const lotus::CallGraph>(
      lotus::buildCallGraph(m_module, *resolver, roots));
  auto aliases = alias_analysis();
  m_graph = std::make_shared<AnalysisGraph>(
      m_module, nullptr,
      [cg, aliases,
       use_alias_targets =
           m_options.call_graph ==
           lotus::CallGraphAnalysisType::OTF](const llvm::CallBase &call) {
        auto found = cg->getCalleesOfCallAt(&call);
        std::vector<const llvm::Function *> functions(found.begin(),
                                                      found.end());
        if (use_alias_targets && !call.getCalledFunction()) {
          std::vector<const llvm::Function *> alias_targets;
          aliases->getIndirectCallTargets(const_cast<llvm::CallBase *>(&call),
                                          alias_targets);
          functions.insert(functions.end(), alias_targets.begin(),
                           alias_targets.end());
        }
        return CalleeTargets{
            std::move(functions),
            llvm::isa<llvm::Function>(
                call.getCalledOperand()->stripPointerCastsAndAliases())};
      },
      [aliases](const llvm::Value *left, const llvm::Value *right) {
        return aliases->mayAlias(left, right);
      });
  return m_graph;
}
void AnalysisSession::invalidate() {
  m_graph.reset();
  m_vtables.reset();
  m_hierarchy.reset();
  m_alias.reset();
}
} // namespace ifds
