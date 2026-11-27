/**
 * @file Andersen.cpp
 * @brief Main implementation of Andersen's flow-insensitive pointer analysis.
 *
 * This file implements the core Andersen pointer analysis algorithm with
 * support for context sensitivity (k-callsite). It orchestrates the three
 * main phases: constraint collection, constraint optimization (HVN/HU), and
 * constraint solving. Also provides command-line options for configuring
 * the analysis behavior.
 *
 * @author rainoftime
 */
#include "Alias/InclusionBased/SparrowAA/Andersen.h"

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/CtxTrait.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/InclusionBased/SparrowAA/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ModuleSlotTracker.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "andersen"

using namespace llvm;

STATISTIC(NumValueNodes, "Number of value nodes created");
STATISTIC(NumConstraints, "Number of constraints collected");
STATISTIC(NumAddrOfConstraints, "Number of addr-of constraints");
STATISTIC(NumCopyConstraints, "Number of copy constraints");
STATISTIC(NumLoadConstraints, "Number of load constraints");
STATISTIC(NumStoreConstraints, "Number of store constraints");
STATISTIC(NumAdaptiveCSFunctions,
          "Functions scored for adaptive context sensitivity");
STATISTIC(NumAdaptiveCSDeepened,
          "Functions that will receive full context depth (adaptive CS)");

// Define option category for Andersen analysis options (non-static so it can be
// used across files)
cl::OptionCategory
    AndersenCategory("Andersen Analysis Options",
                     "Options for configuring Andersen pointer analysis");

// Forward declarations of cl::opts defined in ConstraintOptimize.cpp
extern cl::opt<bool> EnableAdaptiveCS;
extern cl::opt<float> AdaptiveCSThreshold;

cl::opt<bool> DumpDebugInfo("dump-debug",
                            cl::desc("Dump debug info into stderr"),
                            cl::init(false), cl::Hidden,
                            cl::cat(AndersenCategory));
cl::opt<bool> DumpResultInfo("dump-result",
                             cl::desc("Dump result info into stderr"),
                             cl::init(false), cl::Hidden,
                             cl::cat(AndersenCategory));
cl::opt<bool> DumpConstraintInfo("dump-cons",
                                 cl::desc("Dump constraint info into stderr"),
                                 cl::init(false), cl::Hidden,
                                 cl::cat(AndersenCategory));
cl::opt<std::string>
    AndersenDumpConstraintsAfterCollect(
        "andersen-dump-constraints-after-collect",
        cl::desc("Write GPU-oriented binary constraints snapshot immediately "
                 "after collection"),
        cl::init(""), cl::cat(AndersenCategory));
cl::opt<std::string>
    AndersenDumpConstraintsAfterOptimize(
        "andersen-dump-constraints-after-optimize",
        cl::desc("Write GPU-oriented binary constraints snapshot immediately "
                 "after optimization"),
        cl::init(""), cl::cat(AndersenCategory));
cl::opt<unsigned>
    AndersenKContext("andersen-k-cs",
                     cl::desc("Context-sensitive Andersen k-callsite depth "
                              "(k>=0, currently supports 0-32)"),
                     cl::init(0), cl::cat(AndersenCategory));
cl::opt<bool> AndersenUseBDDPointsTo(
    "andersen-use-bdd-pts",
    cl::desc("Use BDD-backed points-to sets instead of SparseBitVector"),
    cl::init(false), cl::cat(AndersenCategory));

namespace {

struct SerializedEdges {
  std::vector<uint32_t> rowOffsets;
  std::vector<uint32_t> columns;
};

// A lightweight, self-contained K-call-site context to avoid depending on
// the buggy KCallSite equality from AserPTA while still honoring the
// requested level of context sensitivity. Contexts are interned so that
// pointer identity is stable and can be used directly as a map key.
template <unsigned K> struct CallStringContext {
  std::array<const llvm::Instruction *, K> sites{};
  uint8_t size = 0;
  bool isGlobal = false;

  static CallStringContext makeInitial(bool globalFlag) {
    CallStringContext ctx;
    ctx.size = 0;
    ctx.isGlobal = globalFlag;
    return ctx;
  }
};

template <unsigned K> struct CallStringContextHash {
  size_t operator()(const CallStringContext<K> &ctx) const {
    auto begin = ctx.sites.begin();
    return llvm::hash_combine(
        ctx.isGlobal, ctx.size,
        llvm::hash_combine_range(begin, begin + ctx.size));
  }
};

template <unsigned K> struct CallStringContextEq {
  bool operator()(const CallStringContext<K> &lhs,
                  const CallStringContext<K> &rhs) const {
    return lhs.isGlobal == rhs.isGlobal && lhs.size == rhs.size &&
           std::equal(lhs.sites.begin(), lhs.sites.begin() + lhs.size,
                      rhs.sites.begin());
  }
};

template <unsigned K> class CallStringCtxManager {
public:
  using Context = CallStringContext<K>;

  CallStringCtxManager() { reset(); }

  const Context *getInitialCtx() const { return initialCtx; }
  const Context *getGlobalCtx() const { return globalCtx; }

  const Context *evolve(const Context *prev, const llvm::Instruction *I) {
    Context next = *prev;
    next.isGlobal = false;
    if (I != nullptr) {
      if (next.size < K) {
        next.sites[next.size++] = I;
      } else {
        std::move(next.sites.begin() + 1, next.sites.end(), next.sites.begin());
        next.sites[K - 1] = I;
      }
    }
    return intern(std::move(next));
  }

  std::string toString(const Context *ctx, bool detailed) const {
    if (ctx == globalCtx)
      return "<global>";
    if (ctx == initialCtx)
      return "<empty>";

    std::string str;
    llvm::raw_string_ostream os(str);
    os << '<';
    for (unsigned i = 0; i < ctx->size; ++i) {
      const llvm::Instruction *I = ctx->sites[i];
      if (I == nullptr)
        continue;
      if (detailed)
        os << *I;
      else
        os << I;
      if (i + 1 < ctx->size)
        os << "->";
    }
    os << '>';
    return os.str();
  }

  void reset() {
    pool.clear();
    initialCtx = intern(Context::makeInitial(false));
    globalCtx = intern(Context::makeInitial(true));
  }

private:
  std::unordered_set<Context, CallStringContextHash<K>,
                     CallStringContextEq<K>> pool;
  const Context *initialCtx = nullptr;
  const Context *globalCtx = nullptr;

  const Context *intern(Context ctx) {
    auto [it, inserted] = pool.insert(std::move(ctx));
    return &*it;
  }
};

template <unsigned K> CallStringCtxManager<K> &getCallStringManager() {
  static CallStringCtxManager<K> manager;
  return manager;
}

template <unsigned K> ContextPolicy buildKCallStringPolicy(const char *name) {
  ContextPolicy policy{};
  policy.initialCtx = +[]() -> ContextPolicy::Context {
    return static_cast<const void *>(getCallStringManager<K>().getInitialCtx());
  };
  policy.globalCtx = +[]() -> ContextPolicy::Context {
    return static_cast<const void *>(getCallStringManager<K>().getGlobalCtx());
  };
  policy.evolve = +[](ContextPolicy::Context prev,
                      const llvm::Instruction *I) -> ContextPolicy::Context {
    return static_cast<const void *>(getCallStringManager<K>().evolve(
        static_cast<const typename CallStringCtxManager<K>::Context *>(prev),
        I));
  };
  policy.toString = +[](ContextPolicy::Context ctx, bool detailed) {
    return getCallStringManager<K>().toString(
        static_cast<const typename CallStringCtxManager<K>::Context *>(ctx),
        detailed);
  };
  policy.release = +[]() {
    // Don't reset the manager to avoid invalidating context pointers
    // that may have been captured by existing Andersen objects.
    // The pool will be cleaned up when the program exits.
  };
  policy.k = K;
  policy.name = name;
  return policy;
}

template <typename Ctx>
ContextPolicy buildCtxPolicy(unsigned k, const char *name) {
  (void)k;
  ContextPolicy policy{};
  policy.initialCtx = +[]() -> ContextPolicy::Context {
    return static_cast<const void *>(aser::CtxTrait<Ctx>::getInitialCtx());
  };
  policy.globalCtx = +[]() -> ContextPolicy::Context {
    return static_cast<const void *>(aser::CtxTrait<Ctx>::getGlobalCtx());
  };
  policy.evolve = +[](ContextPolicy::Context prev,
                      const llvm::Instruction *I) -> ContextPolicy::Context {
    return static_cast<const void *>(
        aser::CtxTrait<Ctx>::contextEvolve(static_cast<const Ctx *>(prev), I));
  };
  policy.toString = +[](ContextPolicy::Context ctx, bool detailed) {
    return aser::CtxTrait<Ctx>::toString(static_cast<const Ctx *>(ctx),
                                         detailed);
  };
  policy.release = +[]() { aser::CtxTrait<Ctx>::release(); };
  policy.k = k;
  policy.name = name;
  return policy;
}

} // namespace

static constexpr unsigned MAX_SUPPORTED_K_CALLSITE = 32;

static ContextPolicy buildSupportedKCallStringPolicy(unsigned kCallSite) {
  switch (kCallSite) {
    case 1:
      return buildKCallStringPolicy<1>("1-CFA");
    case 2:
      return buildKCallStringPolicy<2>("2-CFA");
    case 3:
      return buildKCallStringPolicy<3>("3-CFA");
    case 4:
      return buildKCallStringPolicy<4>("4-CFA");
    case 5:
      return buildKCallStringPolicy<5>("5-CFA");
    case 6:
      return buildKCallStringPolicy<6>("6-CFA");
    case 7:
      return buildKCallStringPolicy<7>("7-CFA");
    case 8:
      return buildKCallStringPolicy<8>("8-CFA");
    case 9:
      return buildKCallStringPolicy<9>("9-CFA");
    case 10:
      return buildKCallStringPolicy<10>("10-CFA");
    case 11:
      return buildKCallStringPolicy<11>("11-CFA");
    case 12:
      return buildKCallStringPolicy<12>("12-CFA");
    case 13:
      return buildKCallStringPolicy<13>("13-CFA");
    case 14:
      return buildKCallStringPolicy<14>("14-CFA");
    case 15:
      return buildKCallStringPolicy<15>("15-CFA");
    case 16:
      return buildKCallStringPolicy<16>("16-CFA");
    case 17:
      return buildKCallStringPolicy<17>("17-CFA");
    case 18:
      return buildKCallStringPolicy<18>("18-CFA");
    case 19:
      return buildKCallStringPolicy<19>("19-CFA");
    case 20:
      return buildKCallStringPolicy<20>("20-CFA");
    case 21:
      return buildKCallStringPolicy<21>("21-CFA");
    case 22:
      return buildKCallStringPolicy<22>("22-CFA");
    case 23:
      return buildKCallStringPolicy<23>("23-CFA");
    case 24:
      return buildKCallStringPolicy<24>("24-CFA");
    case 25:
      return buildKCallStringPolicy<25>("25-CFA");
    case 26:
      return buildKCallStringPolicy<26>("26-CFA");
    case 27:
      return buildKCallStringPolicy<27>("27-CFA");
    case 28:
      return buildKCallStringPolicy<28>("28-CFA");
    case 29:
      return buildKCallStringPolicy<29>("29-CFA");
    case 30:
      return buildKCallStringPolicy<30>("30-CFA");
    case 31:
      return buildKCallStringPolicy<31>("31-CFA");
    case 32:
      return buildKCallStringPolicy<32>("32-CFA");
    default:
      llvm_unreachable("Unsupported k-callsite value in dispatcher");
  }
}

ContextPolicy makeContextPolicy(unsigned kCallSite) {
  if (kCallSite > MAX_SUPPORTED_K_CALLSITE) {
    llvm::errs() << "WARNING: Andersen: k-callsite depth " << kCallSite
                 << " is not supported (max is "
                 << MAX_SUPPORTED_K_CALLSITE
                 << "); falling back to context-insensitive analysis (k=0).\n";
    return buildCtxPolicy<aser::NoCtx>(0, "NoCtx");
  }

  switch (kCallSite) {
    case 0:
      return buildCtxPolicy<aser::NoCtx>(0, "NoCtx");
    default:
      return buildSupportedKCallStringPolicy(kCallSite);
  }
}

// Definition of the static ID member declared in Andersen.h.
// Without this definition any ODR-use of Andersen::ID (e.g. passing it to
// getAnalysis<>()) would cause a linker error.
char Andersen::ID = 0;

ContextPolicy getSelectedAndersenContextPolicy() {
  return makeContextPolicy(AndersenKContext);
}

Andersen::Andersen(const Module &module, ContextPolicy policy)
    : ptsSetImpl(getGlobalPtsSetImpl()), ctxPolicy(policy),
      initialCtx(ctxPolicy.initialCtx()), globalCtx(ctxPolicy.globalCtx()) {
  static std::atomic<int> objectCounter{0};
  int myId = ++objectCounter;
  LOG_INFO("=== Andersen object #{} created ===", myId);
  runOnModule(module);
  LOG_INFO("=== Andersen object #{} analysis completed ===", myId);
}

Andersen::~Andersen() { ctxPolicy.release(); }

AndersNodeFactory::CtxKey
Andersen::evolveContextForCallee(AndersNodeFactory::CtxKey prev,
                                 const llvm::Instruction *I,
                                 const llvm::Function *callee) const {
  if (!EnableAdaptiveCS || contextNeed.empty())
    return ctxPolicy.evolve(prev, I);

  // If the callee's context-need score is below the threshold, keep it
  // context-insensitive (reuse initialCtx) instead of cloning it per caller
  // context.
  if (callee) {
    auto it = contextNeed.find(callee);
    float score = (it != contextNeed.end()) ? it->second : 0.0f;
    if (score < AdaptiveCSThreshold)
      return initialCtx;
  }
  return ctxPolicy.evolve(prev, I);
}

AndersNodeFactory::CtxKey
Andersen::evolveContext(AndersNodeFactory::CtxKey prev,
                        const llvm::Instruction *I) const {
  const llvm::Function *callee = nullptr;
  if (I) {
    if (const auto *cb = llvm::dyn_cast<llvm::CallBase>(I))
      callee = cb->getCalledFunction();
  }
  return evolveContextForCallee(prev, I, callee);
}

// Compute per-function context-need scores using the peLabel map captured from
// HVN and the pre-rewrite argument-passing edges (formal <- actual). Using
// pre-rewrite edges ensures that actuals rewritten from COPY to ADDR_OF or
// formals merged during HVN are not lost.
void Andersen::computeContextNeed(
    const std::vector<FormalActualEdge> &argEdges) {
  contextNeed.clear();
  // Map: function → (formal node → list of peLabels from callers)
  llvm::DenseMap<const llvm::Function *,
                 llvm::DenseMap<NodeIndex, llvm::SmallVector<unsigned, 4>>>
      paramLabels;

  for (const auto &edge : argEdges) {
    auto labelIt = hvnPELabels.find(edge.actual);
    if (labelIt == hvnPELabels.end())
      labelIt = hvnPELabels.find(nodeFactory.getMergeTarget(edge.actual));
    unsigned label = labelIt != hvnPELabels.end() ? labelIt->second : 0;
    // label == 0 means non-pointer; skip
    if (label == 0)
      continue;
    paramLabels[edge.func][edge.formal].push_back(label);
  }

  for (auto &[F, params] : paramLabels) {
    int diverse = 0, total = 0;
    for (auto &[param, labels] : params) {
      ++total;
      // Check if more than one distinct label appears
      llvm::SmallVector<unsigned, 4> sorted = labels;
      std::sort(sorted.begin(), sorted.end());
      if (std::unique(sorted.begin(), sorted.end()) != sorted.begin() + 1)
        ++diverse;
    }
    float score = total > 0 ? static_cast<float>(diverse) / total : 0.0f;
    contextNeed[F] = score;
    ++NumAdaptiveCSFunctions;
    if (score >= AdaptiveCSThreshold)
      ++NumAdaptiveCSDeepened;
    LOG_DEBUG("adaptive-cs: {} score={:.2f} ({}/{} diverse params)",
              F->getName().str(), score, diverse, total);
  }
}

void Andersen::getAllAllocationSites(
    std::vector<const llvm::Value *> &allocSites) const {
  nodeFactory.getAllocSites(allocSites);
}

bool Andersen::getPointsToSet(const llvm::Value *v,
                              std::vector<const llvm::Value *> &ptsSet) const {
  AndersPtsSet aggregated;
  if (!getPointsToSet(v, aggregated))
    return false;

  ptsSet.clear();
  DenseSet<const llvm::Value *> uniq;
  for (auto idx : aggregated) {
    if (idx == nodeFactory.getNullObjectNode())
      continue;
    const llvm::Value *val = nodeFactory.getValueForNode(idx);
    if (val && uniq.insert(val).second) {
      ptsSet.push_back(val);
    }
  }
  return true;
}

bool Andersen::getPointsToSet(const llvm::Value *v,
                              AndersPtsSet &ptsSet) const {
  std::vector<NodeIndex> nodes;
  nodeFactory.getValueNodesFor(v, nodes);
  if (nodes.empty())
    return false;

  ptsSet.clear();
  bool sawUnknown = false;
  bool sawKnown = false;
  for (auto n : nodes) {
    if (n == AndersNodeFactory::InvalidIndex ||
        n == nodeFactory.getUniversalPtrNode()) {
      sawUnknown = true;
      continue;
    }
    NodeIndex rep = nodeFactory.getMergeTarget(n);
    if (ptsSetImpl == PtsSetImpl::SPARSE_BITVECTOR) {
      if (rep >= sparsePtsGraph.size() || sparsePtsGraph[rep].isEmpty())
        continue;
      sawKnown = true;
      ptsSet.unionWith(sparsePtsGraph[rep]);
    } else {
      if (rep >= bddPtsGraph.size() || bddPtsGraph[rep].isEmpty())
        continue;
      sawKnown = true;
      for (auto idx : bddPtsGraph[rep])
        ptsSet.insert(static_cast<unsigned>(idx));
    }
  }

  if (sawUnknown)
    ptsSet.insert(static_cast<unsigned>(nodeFactory.getUniversalObjNode()));

  // Return true if we found any information (known nodes or universal ptr).
  return sawKnown || sawUnknown;
}

bool Andersen::getPointsToSetInContext(const llvm::Value *v,
                                       AndersNodeFactory::CtxKey ctx,
                                       AndersPtsSet &ptsSet) const {
  NodeIndex n = nodeFactory.getValueNodeFor(v, ctx);
  if (n == AndersNodeFactory::InvalidIndex)
    return false;

  if (n == nodeFactory.getUniversalPtrNode()) {
    ptsSet.clear();
    ptsSet.insert(static_cast<unsigned>(nodeFactory.getUniversalObjNode()));
    return true;
  }

  NodeIndex rep = nodeFactory.getMergeTarget(n);
  if (ptsSetImpl == PtsSetImpl::SPARSE_BITVECTOR) {
    if (rep >= sparsePtsGraph.size() || sparsePtsGraph[rep].isEmpty())
      return false;
    ptsSet = sparsePtsGraph[rep];
    return true;
  }

  if (rep >= bddPtsGraph.size() || bddPtsGraph[rep].isEmpty())
    return false;
  ptsSet.clear();
  for (auto idx : bddPtsGraph[rep])
    ptsSet.insert(static_cast<unsigned>(idx));
  return true;
}

bool Andersen::getPointsToSetInContext(
    const llvm::Value *v, AndersNodeFactory::CtxKey ctx,
    std::vector<const llvm::Value *> &ptsSet) const {
  AndersPtsSet aggregated;
  if (!getPointsToSetInContext(v, ctx, aggregated))
    return false;

  ptsSet.clear();
  llvm::DenseSet<const llvm::Value *> uniq;
  for (auto idx : aggregated) {
    if (idx == nodeFactory.getNullObjectNode())
      continue;
    const llvm::Value *val = nodeFactory.getValueForNode(idx);
    if (val && uniq.insert(val).second) {
      ptsSet.push_back(val);
    }
  }
  return true;
}

bool Andersen::runOnModule(const Module &M) {
  LOG_INFO("Starting Andersen analysis on module: {}", M.getName().str());
  stats = Stats{};

  // When adaptive CS is requested with k>0, we do a lightweight two-phase
  // collection:
  //   Phase 0: collect CI constraints (k=0) → run HVN → score functions
  //   Phase 1: reset, collect full CS constraints using the adaptive evolve
  // When adaptive CS is off, or k=0, we do the single-phase flow as before.
  if (EnableAdaptiveCS && ctxPolicy.k > 0) {
    LOG_INFO("Adaptive CS: running CI pre-pass to compute context-need scores");

    // Phase 0: collect with a temporary CI policy
    ContextPolicy ciPolicy = makeContextPolicy(0);
    ContextPolicy savedPolicy = ctxPolicy;
    ctxPolicy = ciPolicy;
    initialCtx = ctxPolicy.initialCtx();
    globalCtx = ctxPolicy.globalCtx();
    visitedFunctions.clear();
    collectConstraints(M);

    // Capture formal <- actual edges BEFORE HVN merges nodes or rewrites
    // COPY(formal, &x) into ADDR_OF(formal, x).
    std::vector<FormalActualEdge> argEdges;
    for (const auto &c : constraints) {
      if (c.getType() != AndersConstraint::COPY)
        continue;
      NodeIndex dest = c.getDest();
      const llvm::Value *val = nodeFactory.getValueForNode(dest);
      if (const auto *arg = llvm::dyn_cast_or_null<llvm::Argument>(val))
        argEdges.push_back({arg->getParent(), dest, c.getSrc()});
    }

    // Run HVN on CI constraints and score. We don't keep the rewritten CI
    // constraints — they're discarded after scoring.
    runHVNAndCapturePELabels(constraints, nodeFactory, hvnPELabels);
    computeContextNeed(argEdges);
    LOG_INFO("Adaptive CS: scored {} functions", contextNeed.size());

    // Phase 1: reset state and re-collect using the adaptive CS policy
    constraints.clear();
    nodeFactory = AndersNodeFactory();
    sparsePtsGraph.clear();
    bddPtsGraph.clear();
    ctxPolicy = savedPolicy;
    initialCtx = ctxPolicy.initialCtx();
    globalCtx = ctxPolicy.globalCtx();
    visitedFunctions.clear();
    LOG_INFO(
        "Adaptive CS: re-collecting constraints with adaptive context policy");
  }

  visitedFunctions.clear();
  collectConstraints(M);
  if (!AndersenDumpConstraintsAfterCollect.empty())
    serializeConstraints(AndersenDumpConstraintsAfterCollect,
                         sparrow_aa::SnapshotPhase::Collect);

  // Update statistics after constraint collection
  size_t numConstraints = constraints.size();
  size_t numValueNodes = nodeFactory.getNumNodes();
  stats.numNodes = numValueNodes;
  stats.numConstraintsBeforeOpt = numConstraints;
  NumConstraints = numConstraints;
  NumValueNodes = numValueNodes;
  LOG_INFO("Collected {} constraints and created {} value nodes",
           numConstraints, numValueNodes);
  for (const auto &c : constraints) {
    switch (c.getType()) {
    case AndersConstraint::ADDR_OF:
      ++NumAddrOfConstraints;
      break;
    case AndersConstraint::COPY:
      ++NumCopyConstraints;
      break;
    case AndersConstraint::LOAD:
      ++NumLoadConstraints;
      break;
    case AndersConstraint::STORE:
      ++NumStoreConstraints;
      break;
    }
  }

  if (DumpDebugInfo)
    dumpConstraintsPlainVanilla();

  LOG_INFO("Starting constraint optimization...");
  optimizeConstraints();
  stats.numConstraintsAfterOpt = constraints.size();
  LOG_INFO("Constraint optimization completed");
  if (!AndersenDumpConstraintsAfterOptimize.empty())
    serializeConstraints(AndersenDumpConstraintsAfterOptimize,
                         sparrow_aa::SnapshotPhase::Optimize);

  if (DumpConstraintInfo)
    dumpConstraints();

  LOG_INFO("Starting constraint solving...");
  solveConstraints();
  LOG_INFO("Andersen analysis completed successfully");

  if (DumpDebugInfo) {
    LOG_DEBUG("");
    dumpPtsGraphPlainVanilla();
  }

  if (DumpResultInfo) {
    nodeFactory.dumpNodeInfo();
    LOG_DEBUG("");
    dumpPtsGraphPlainVanilla();
  }

  return false;
}

void Andersen::dumpConstraint(const AndersConstraint &item) const {
  NodeIndex dest = item.getDest();
  NodeIndex src = item.getSrc();

  switch (item.getType()) {
  case AndersConstraint::COPY: {
    nodeFactory.dumpNode(dest);
    errs() << " = ";
    nodeFactory.dumpNode(src);
    break;
  }
  case AndersConstraint::LOAD: {
    nodeFactory.dumpNode(dest);
    errs() << " = *";
    nodeFactory.dumpNode(src);
    break;
  }
  case AndersConstraint::STORE: {
    errs() << "*";
    nodeFactory.dumpNode(dest);
    errs() << " = ";
    nodeFactory.dumpNode(src);
    break;
  }
  case AndersConstraint::ADDR_OF: {
    nodeFactory.dumpNode(dest);
    errs() << " = &";
    nodeFactory.dumpNode(src);
  }
  }

  errs() << "\n";
}

void Andersen::dumpConstraints() const {
  LOG_DEBUG("\n----- Constraints -----");
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
  for (auto const &item : constraints)
    dumpConstraint(item);
#endif
  LOG_DEBUG("----- End of Print -----");
}

void Andersen::dumpConstraintsPlainVanilla() const {
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
  for (auto const &item : constraints) {
    LOG_DEBUG("{} {} {} 0", item.getType(), item.getDest(), item.getSrc());
  }
#endif
}

void Andersen::dumpPtsGraphPlainVanilla() const {
  for (unsigned i = 0, e = nodeFactory.getNumNodes(); i < e; ++i) {
    NodeIndex rep = nodeFactory.getMergeTarget(i);
    if (ptsSetImpl == PtsSetImpl::SPARSE_BITVECTOR) {
      if (rep < sparsePtsGraph.size() && !sparsePtsGraph[rep].isEmpty()) {
        std::stringstream ss;
        ss << i;
        for (auto v : sparsePtsGraph[rep])
          ss << " " << v;
        LOG_DEBUG("{}", ss.str());
      }
    } else {
      if (rep < bddPtsGraph.size() && !bddPtsGraph[rep].isEmpty()) {
        std::stringstream ss;
        ss << i;
        for (auto v : bddPtsGraph[rep])
          ss << " " << v;
        LOG_DEBUG("{}", ss.str());
      }
    }
  }
}

void Andersen::serializeConstraints(
    StringRef outputPath, sparrow_aa::SnapshotPhase phase) const {
  SmallString<256> path(outputPath);
  if (path.empty())
    return;

  SmallString<256> parentPath(sys::path::parent_path(path));
  if (!parentPath.empty())
    sys::fs::create_directories(parentPath);

  std::error_code errInfo;
  ToolOutputFile outFile(path, errInfo, sys::fs::OF_Text);
  if (errInfo) {
    LOG_ERROR("Failed to open constraint dump '{}': {}", outputPath.str(),
              errInfo.message());
    return;
  }

  const Module *ownerModule = nullptr;
  for (unsigned i = 0, e = nodeFactory.getNumNodes(); i < e && !ownerModule;
       ++i) {
    const llvm::Value *val = nodeFactory.getValueForNode(i);
    if (const auto *gv = llvm::dyn_cast_or_null<llvm::GlobalValue>(val))
      ownerModule = gv->getParent();
    else if (const auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(val))
      ownerModule = inst->getModule();
  }
  ModuleSlotTracker mst(ownerModule);

  auto formatValue = [&](const llvm::Value *value) -> std::string {
    if (value == nullptr)
      return "-";
    if (value->hasName())
      return value->getName().str();

    std::string rendered;
    raw_string_ostream os(rendered);
    if (ownerModule)
      value->printAsOperand(os, false, mst);
    else
      value->printAsOperand(os, false);
    os.flush();
    return rendered;
  };
  auto internString = [](llvm::StringRef value, std::string &blob,
                         llvm::StringMap<uint32_t> &offsets) -> uint32_t {
    auto it = offsets.find(value);
    if (it != offsets.end())
      return it->second;
    uint32_t offset = static_cast<uint32_t>(blob.size());
    blob.append(value.data(), value.size());
    blob.push_back('\0');
    offsets[value] = offset;
    return offset;
  };

  std::map<AndersNodeFactory::CtxKey, unsigned> contextIds;
  for (unsigned i = 0, e = nodeFactory.getNumNodes(); i < e; ++i) {
    const auto *ctx = nodeFactory.getContextForNode(i);
    if (ctx == nullptr)
      continue;
    contextIds.try_emplace(ctx, contextIds.size());
  }

  std::vector<AndersConstraint> stableConstraints = constraints;
  std::stable_sort(stableConstraints.begin(), stableConstraints.end());

  std::string stringBlob;
  llvm::StringMap<uint32_t> stringOffsets;
  internString(ctxPolicy.name, stringBlob, stringOffsets);

  std::vector<sparrow_aa::SnapshotContextRecord> contextRecords;
  contextRecords.reserve(contextIds.size());
  for (const auto &entry : contextIds) {
    std::string label = contextToString(entry.first, false);
    uint32_t labelOffset = internString(label, stringBlob, stringOffsets);
    contextRecords.push_back({entry.second, labelOffset});
  }
  std::sort(contextRecords.begin(), contextRecords.end(),
            [](const sparrow_aa::SnapshotContextRecord &lhs,
               const sparrow_aa::SnapshotContextRecord &rhs) {
              return lhs.context_id < rhs.context_id;
            });

  std::vector<sparrow_aa::SnapshotNodeRecord> nodeRecords;
  nodeRecords.reserve(nodeFactory.getNumNodes());
  for (unsigned i = 0, e = nodeFactory.getNumNodes(); i < e; ++i) {
    const auto *ctx = nodeFactory.getContextForNode(i);
    uint32_t contextId =
        ctx == nullptr ? sparrow_aa::kInvalidContextId
                       : static_cast<uint32_t>(contextIds.at(ctx));
    uint32_t valueOffset =
        internString(formatValue(nodeFactory.getValueForNode(i)), stringBlob,
                     stringOffsets);
    nodeRecords.push_back(
        {i,
         nodeFactory.getMergeTarget(i),
         contextId,
         static_cast<uint16_t>(nodeFactory.getTypeForNode(i)),
         static_cast<uint16_t>(nodeFactory.getRoleForNode(i)),
         valueOffset});
  }

  auto buildEdgePartition = [&](AndersConstraint::ConstraintType type) {
    SerializedEdges edges;
    edges.rowOffsets.assign(nodeFactory.getNumNodes() + 1, 0);
    for (const auto &constraint : stableConstraints) {
      if (constraint.getType() != type)
        continue;
      ++edges.rowOffsets[constraint.getDest() + 1];
    }
    for (size_t i = 1; i < edges.rowOffsets.size(); ++i)
      edges.rowOffsets[i] += edges.rowOffsets[i - 1];

    edges.columns.resize(edges.rowOffsets.back());
    std::vector<uint32_t> cursor = edges.rowOffsets;
    for (const auto &constraint : stableConstraints) {
      if (constraint.getType() != type)
        continue;
      edges.columns[cursor[constraint.getDest()]++] = constraint.getSrc();
    }
    return edges;
  };

  SerializedEdges addrOfEdges = buildEdgePartition(AndersConstraint::ADDR_OF);
  SerializedEdges copyEdges = buildEdgePartition(AndersConstraint::COPY);
  SerializedEdges loadEdges = buildEdgePartition(AndersConstraint::LOAD);
  SerializedEdges storeEdges = buildEdgePartition(AndersConstraint::STORE);

  std::vector<sparrow_aa::SnapshotSectionHeader> sections;
  sections.reserve(11);
  uint64_t offset = sizeof(sparrow_aa::SnapshotHeader) +
                    sizeof(sparrow_aa::SnapshotSectionHeader) * 11;
  auto addSection = [&](sparrow_aa::SnapshotSectionKind kind, uint64_t byteSize,
                        uint64_t elementCount) {
    sections.push_back(
        {static_cast<uint32_t>(kind), 0, offset, byteSize, elementCount});
    offset += byteSize;
  };

  addSection(sparrow_aa::SnapshotSectionKind::Contexts,
             sizeof(sparrow_aa::SnapshotContextRecord) * contextRecords.size(),
             contextRecords.size());
  addSection(sparrow_aa::SnapshotSectionKind::Nodes,
             sizeof(sparrow_aa::SnapshotNodeRecord) * nodeRecords.size(),
             nodeRecords.size());
  addSection(sparrow_aa::SnapshotSectionKind::Strings, stringBlob.size(),
             stringBlob.size());
  addSection(sparrow_aa::SnapshotSectionKind::AddrOfRowOffsets,
             sizeof(uint32_t) * addrOfEdges.rowOffsets.size(),
             addrOfEdges.rowOffsets.size());
  addSection(sparrow_aa::SnapshotSectionKind::AddrOfColumns,
             sizeof(uint32_t) * addrOfEdges.columns.size(),
             addrOfEdges.columns.size());
  addSection(sparrow_aa::SnapshotSectionKind::CopyRowOffsets,
             sizeof(uint32_t) * copyEdges.rowOffsets.size(),
             copyEdges.rowOffsets.size());
  addSection(sparrow_aa::SnapshotSectionKind::CopyColumns,
             sizeof(uint32_t) * copyEdges.columns.size(),
             copyEdges.columns.size());
  addSection(sparrow_aa::SnapshotSectionKind::LoadRowOffsets,
             sizeof(uint32_t) * loadEdges.rowOffsets.size(),
             loadEdges.rowOffsets.size());
  addSection(sparrow_aa::SnapshotSectionKind::LoadColumns,
             sizeof(uint32_t) * loadEdges.columns.size(),
             loadEdges.columns.size());
  addSection(sparrow_aa::SnapshotSectionKind::StoreRowOffsets,
             sizeof(uint32_t) * storeEdges.rowOffsets.size(),
             storeEdges.rowOffsets.size());
  addSection(sparrow_aa::SnapshotSectionKind::StoreColumns,
             sizeof(uint32_t) * storeEdges.columns.size(),
             storeEdges.columns.size());

  sparrow_aa::SnapshotHeader header{};
  std::memcpy(header.magic, "SPAA2BIN", 8);
  header.version = sparrow_aa::kSnapshotVersion;
  header.phase = static_cast<uint32_t>(phase);
  header.node_count = nodeFactory.getNumNodes();
  header.context_count = contextRecords.size();
  header.string_bytes = stringBlob.size();
  header.section_count = sections.size();
  header.section_table_offset = sizeof(sparrow_aa::SnapshotHeader);

  raw_fd_ostream &os = outFile.os();
  os.write(reinterpret_cast<const char *>(&header), sizeof(header));
  os.write(reinterpret_cast<const char *>(sections.data()),
           sizeof(sparrow_aa::SnapshotSectionHeader) * sections.size());
  if (!contextRecords.empty()) {
    os.write(reinterpret_cast<const char *>(contextRecords.data()),
             sizeof(sparrow_aa::SnapshotContextRecord) * contextRecords.size());
  }
  if (!nodeRecords.empty()) {
    os.write(reinterpret_cast<const char *>(nodeRecords.data()),
             sizeof(sparrow_aa::SnapshotNodeRecord) * nodeRecords.size());
  }
  if (!stringBlob.empty())
    os.write(stringBlob.data(), stringBlob.size());

  auto writeUInt32Vector = [&](const std::vector<uint32_t> &values) {
    if (!values.empty()) {
      os.write(reinterpret_cast<const char *>(values.data()),
               sizeof(uint32_t) * values.size());
    }
  };
  writeUInt32Vector(addrOfEdges.rowOffsets);
  writeUInt32Vector(addrOfEdges.columns);
  writeUInt32Vector(copyEdges.rowOffsets);
  writeUInt32Vector(copyEdges.columns);
  writeUInt32Vector(loadEdges.rowOffsets);
  writeUInt32Vector(loadEdges.columns);
  writeUInt32Vector(storeEdges.rowOffsets);
  writeUInt32Vector(storeEdges.columns);

  outFile.keep();
}
