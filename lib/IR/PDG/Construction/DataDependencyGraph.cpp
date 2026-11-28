#include "llvm/IR/InstIterator.h"
#include "llvm/Support/CommandLine.h"

#include "IR/PDG/Construction/GraphBuilder.h"
/**
 * @file DataDependencyGraph.cpp
 * @brief Implementation of the data dependency analysis for the PDG
 *
 * This file implements the DataDependencyGraph pass, which analyzes data
 * dependencies between program elements. Data dependencies occur when one
 * instruction defines a value that is used by another instruction (def-use
 * chains).
 *
 * Key features:
 * - Analysis of def-use chains in LLVM IR
 * - Support for different types of data dependencies (direct, memory, etc.)
 * - Function-level data dependency analysis
 * - Integration with the overall PDG framework
 * - Support for memory-based dependencies through load/store analysis
 *
 * The data dependency analysis is a fundamental component of the PDG system,
 * complementing control dependency analysis to provide a complete view of
 * program dependencies.
 */

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "IR/PDG/Construction/DataDependencyGraph.h"
#include "IR/PDG/Core/PDGCommandLineOptions.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

using llvm::cl::desc;
using llvm::cl::init;
using llvm::cl::opt;

namespace {
// Fast filter: AliasAnalysisWrapper::query only answers alias queries when both
// values have pointer type (all non-pointer instructions return NoAlias).
static bool isAliasRelevantInst(const llvm::Instruction &I) {
  return I.getType()->isPointerTy();
}

// Command-line knobs to choose alias analyses for data dependence construction.
// -pdg-aa : over-approximate (sound) AA used to add alias edges (default:
// Andersen). -pdg-aa-under : optional under-approximate AA used to restrict
// alias edges to proven must-alias pairs (default: none).
static opt<std::string> PdgAliasOverOpt(
    "pdg-aa",
    desc("Alias analysis used for PDG data deps "
         "(andersen, andersen-1cfa, andersen-2cfa, dyck, cfl-anders, "
         "cfl-steens, combined, underapprox, none)"),
    init("andersen"));

static opt<std::string> PdgAliasUnderOpt(
    "pdg-aa-under",
    desc("Under-approximate alias analysis for must-alias filtering "
         "(none|underapprox)"),
    init("none"));

// Map a user-facing string to an AAConfig. Defaults to the provided fallback
// when the string is unknown.
static lotus::AAConfig parseAAConfig(const std::string &aa,
                                     const lotus::AAConfig &fallback) {
  return lotus::parseAAConfigFromString(aa, fallback);
}

// Helper that builds an alias wrapper or returns nullptr when disabled/failed.
static std::unique_ptr<lotus::AliasAnalysisWrapper>
buildAliasWrapper(llvm::Module &M, const std::string &userChoice,
                  const lotus::AAConfig &fallback, const char *label) {
  std::string lower = userChoice;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  if (lower == "none" || lower == "off" || lower == "disable") {
    if (pdg::DEBUG)
      llvm::errs() << "pdg: " << label << " alias analysis disabled by flag\n";
    return nullptr;
  }

  auto config = parseAAConfig(userChoice, fallback);
  // AliasAnalysisWrapper::initialize() leaves _tli null for CFLSteens, which
  // dereferences null during construction. Guard cleanly here without touching
  // lib/Alias/.
  if (config.impl == lotus::AAConfig::Implementation::CFLSteens) {
    llvm::errs() << "pdg: alias analysis 'cfl-steens' is unavailable "
                    "(AliasAnalysisWrapper does not initialize TLI for "
                    "CFLSteens); disabling "
                 << label << " alias analysis\n";
    return nullptr;
  }

  auto wrapper = lotus::AliasAnalysisFactory::create(M, config);

  if (!wrapper || !wrapper->isInitialized()) {
    llvm::errs() << "pdg: failed to initialize " << label << " alias analysis: "
                 << lotus::AliasAnalysisFactory::getTypeName(config) << "\n";
    return nullptr;
  }

  if (pdg::DEBUG)
    llvm::errs() << "pdg: using "
                 << lotus::AliasAnalysisFactory::getTypeName(config) << " for "
                 << label << " alias queries\n";

  return wrapper;
}
} // namespace

char pdg::DataDependencyGraph::ID = 0;

using namespace llvm;

bool pdg::DataDependencyGraph::runOnModule(Module &M) {
  ProgramGraph &g = ProgramGraph::getInstance();
  // Each dependence-pass invocation owns a fresh construction cycle. A raw
  // module address cannot identify cached IR across mutation or destruction.
  pdg::GraphBuilder(g).build(M);
  pdg::GraphBuilder(g).bindDebugInfo(M);

  // Initialize alias analysis wrappers based on command-line choices.
  _alias_wrapper_over =
      buildAliasWrapper(M, PdgAliasOverOpt.getValue(),
                        lotus::AAConfig::SparrowAA_NoCtx(), "over-approximate");
  _alias_wrapper_under =
      buildAliasWrapper(M, PdgAliasUnderOpt.getValue(),
                        lotus::AAConfig::UnderApprox(), "under-approximate");

  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    _mem_dep_res = &getAnalysis<MemoryDependenceWrapperPass>(F).getMemDep();
    for (auto inst_iter = inst_begin(F); inst_iter != inst_end(F);
         inst_iter++) {
      addDefUseEdges(*inst_iter);
      addRAWEdges(*inst_iter);
    }
    // Build alias edges once per function (not once per instruction) to avoid
    // O(n³) complexity.  Also guarded against AA-unavailable blowup.
    addAliasEdgesForFunction(F);
  }
  return false;
}

void pdg::DataDependencyGraph::addAliasEdgesForFunction(Function &F) {
  // Guard against AA-unavailable conservative MayAlias blowup.
  if (!_alias_wrapper_over || !_alias_wrapper_over->isInitialized()) {
    if (pdg::DEBUG)
      llvm::errs() << "pdg: skipping alias edges for " << F.getName()
                   << " (over-approximate AA not available)\n";
    return;
  }

  ProgramGraph &g = ProgramGraph::getInstance();

  // Collect pointer-producing instructions and group by stripPointerCasts() so
  // bitcasts/no-op GEPs share one representative AA query.
  struct PtrGroup {
    const Value *rep;
    llvm::SmallVector<std::pair<Instruction *, Node *>, 4> members;
  };
  llvm::DenseMap<const Value *, unsigned> canon_to_group;
  std::vector<PtrGroup> groups;
  for (auto inst_iter = inst_begin(F); inst_iter != inst_end(F); ++inst_iter) {
    Instruction &I = *inst_iter;
    if (!isAliasRelevantInst(I))
      continue;
    Node *n = g.getNode(I);
    if (!n)
      continue;
    const Value *canon = I.stripPointerCasts();
    if (!canon || !canon->getType()->isPointerTy())
      canon = &I;
    auto res = canon_to_group.try_emplace(canon, groups.size());
    if (res.second) {
      PtrGroup group;
      group.rep = canon;
      group.members.push_back({&I, n});
      groups.push_back(std::move(group));
    } else {
      groups[res.first->second].members.push_back({&I, n});
    }
  }

  if (groups.empty())
    return;

  const bool filter_must_only =
      (_alias_wrapper_under && _alias_wrapper_under->isInitialized());

  auto connectGroups = [&](unsigned lhs, unsigned rhs, bool known_must) {
    if (lhs == rhs)
      return;
    const auto &g1 = groups[lhs];
    const auto &g2 = groups[rhs];
    if (filter_must_only && !known_must) {
      auto under = queryAliasUnderApproximate(
          *const_cast<Value *>(g1.rep), *const_cast<Value *>(g2.rep));
      if (under != llvm::AliasResult::MustAlias)
        return;
    }
    for (const auto &m1 : g1.members) {
      for (const auto &m2 : g2.members) {
        m1.second->addNeighbor(*m2.second, EdgeType::DATA_ALIAS);
        m2.second->addNeighbor(*m1.second, EdgeType::DATA_ALIAS);
      }
    }
  };

  // Intra-group edges: pointers sharing the same stripPointerCasts() base
  // always must-alias each other.
  for (const auto &grp : groups) {
    if (grp.members.size() <= 1)
      continue;
    for (size_t i = 0; i < grp.members.size(); ++i) {
      for (size_t j = i + 1; j < grp.members.size(); ++j) {
        grp.members[i].second->addNeighbor(*grp.members[j].second,
                                           EdgeType::DATA_ALIAS);
        grp.members[j].second->addNeighbor(*grp.members[i].second,
                                           EdgeType::DATA_ALIAS);
      }
    }
  }

  if (groups.size() <= 1)
    return;

  const auto impl = _alias_wrapper_over->getConfig().impl;
  const bool supports_alias_set =
      (impl == lotus::AAConfig::Implementation::DyckAA);

  if (supports_alias_set) {
    // DyckAA is unification-based: values in the same equivalence class alias
    // each other, and values without an alias set conservatively alias all
    // other pointers (matching DyckAliasAnalysis::mayAlias).
    std::vector<bool> visited(groups.size(), false);
    std::vector<bool> untracked(groups.size(), false);
    std::vector<const Value *> alias_set;
    for (unsigned i = 0; i < groups.size(); ++i) {
      if (visited[i])
        continue;
      visited[i] = true;
      alias_set.clear();
      if (!_alias_wrapper_over->getAliasSet(groups[i].rep, alias_set)) {
        untracked[i] = true;
        continue;
      }
      llvm::SmallVector<unsigned, 16> local_class;
      local_class.push_back(i);
      for (const Value *member : alias_set) {
        if (!member)
          continue;
        auto it = canon_to_group.find(member);
        if (it == canon_to_group.end())
          it = canon_to_group.find(member->stripPointerCasts());
        if (it != canon_to_group.end() && !visited[it->second]) {
          visited[it->second] = true;
          local_class.push_back(it->second);
        }
      }
      if (local_class.size() <= 1)
        continue;
      for (size_t a = 0; a < local_class.size(); ++a) {
        for (size_t b = a + 1; b < local_class.size(); ++b) {
          connectGroups(local_class[a], local_class[b], false);
        }
      }
    }
    for (unsigned i = 0; i < groups.size(); ++i) {
      if (!untracked[i])
        continue;
      for (unsigned j = 0; j < groups.size(); ++j) {
        if (j == i || (j < i && untracked[j]))
          continue;
        connectGroups(i, j, false);
      }
    }
    return;
  }

  // Exact pairwise queries over deduplicated stripPointerCasts() groups.
  for (unsigned i = 0; i < groups.size(); ++i) {
    for (unsigned j = i + 1; j < groups.size(); ++j) {
      auto over_result = queryAliasOverApproximate(
          *const_cast<Value *>(groups[i].rep),
          *const_cast<Value *>(groups[j].rep));
      if (over_result == llvm::AliasResult::NoAlias)
        continue;
      connectGroups(i, j, over_result == llvm::AliasResult::MustAlias);
    }
  }
}

void pdg::DataDependencyGraph::addDefUseEdges(Instruction &inst) {
  ProgramGraph &g = ProgramGraph::getInstance();
  for (auto *user : inst.users()) {
    Node *src = g.getNode(inst);
    Node *dst = g.getNode(*user);
    if (src == nullptr || dst == nullptr)
      continue;
    EdgeType edge_type = EdgeType::DATA_DEF_USE;
    if (dst->getNodeType() == GraphNodeType::ANNO_VAR)
      edge_type = EdgeType::ANNO_VAR;
    if (dst->getNodeType() == GraphNodeType::ANNO_GLOBAL)
      edge_type = EdgeType::ANNO_GLOBAL;
    src->addNeighbor(*dst, edge_type);
  }
}

void pdg::DataDependencyGraph::addRAWEdges(Instruction &inst) {
  if (!isa<LoadInst>(&inst))
    return;

  ProgramGraph &g = ProgramGraph::getInstance();
  auto dep_res = _mem_dep_res->getDependency(&inst);
  auto *dep_inst = dep_res.getInst();

  if (dep_inst && dep_inst != &inst && dep_inst->mayWriteToMemory()) {
    Node *src = g.getNode(inst);
    Node *dst = g.getNode(*dep_inst);
    if (src != nullptr && dst != nullptr)
      dst->addNeighbor(*src, EdgeType::DATA_RAW);
  }

  // Non-local dependencies: walk defs/clobbers in other blocks.
  llvm::SmallVector<llvm::NonLocalDepResult, 8> non_local_deps;
  _mem_dep_res->getNonLocalPointerDependency(&inst, non_local_deps);
  for (auto &dep : non_local_deps) {
    auto res = dep.getResult();
    if (!res.isDef() && !res.isClobber())
      continue;
    Instruction *nl_inst = res.getInst();
    if (!nl_inst || nl_inst == &inst || !nl_inst->mayWriteToMemory())
      continue;
    Node *src = g.getNode(inst);
    Node *dst = g.getNode(*nl_inst);
    if (src != nullptr && dst != nullptr)
      dst->addNeighbor(*src, EdgeType::DATA_RAW);
  }
}

llvm::AliasResult
pdg::DataDependencyGraph::queryAliasUnderApproximate(llvm::Value &v1,
                                                     llvm::Value &v2) {
  // Use the under-approximation wrapper (syntactic pattern matching)
  // This only returns MustAlias for clear syntactic patterns, otherwise NoAlias
  if (_alias_wrapper_under && _alias_wrapper_under->isInitialized()) {
    return _alias_wrapper_under->query(&v1, &v2);
  }

  // Fall back to simple check if wrapper is not available
  if (!v1.getType()->isPointerTy() || !v2.getType()->isPointerTy())
    return llvm::AliasResult::NoAlias;

  return llvm::AliasResult::NoAlias;
}

llvm::AliasResult
pdg::DataDependencyGraph::queryAliasOverApproximate(llvm::Value &v1,
                                                    llvm::Value &v2) {
  // Use the over-approximation wrapper (Andersen's analysis)
  // This integrates precise pointer analysis from lib/Alias/InclusionBased/SparrowAA
  if (_alias_wrapper_over && _alias_wrapper_over->isInitialized()) {
    return _alias_wrapper_over->query(&v1, &v2);
  }

  // Wrapper disabled or failed to initialize: stay conservative.
  return llvm::AliasResult::MayAlias;
}

void pdg::DataDependencyGraph::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<MemoryDependenceWrapperPass>();
  AU.setPreservesAll();
}

static RegisterPass<pdg::DataDependencyGraph>
    DDG("ddg", "Data Dependency Graph Construction", false, true);
