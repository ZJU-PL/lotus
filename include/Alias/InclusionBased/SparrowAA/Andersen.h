/**
 * @file Andersen.h
 * @brief Andersen-style interprocedural points-to analysis (SparrowAA).
 *
 * ## Algorithm Overview
 *
 * This implements **Andersen's subset-based, flow-insensitive** pointer
 * analysis.  Context sensitivity is configurable at construction time:
 * the default is context-insensitive (k=0), but k-call-site (k-CFA)
 * contexts can be enabled via `makeContextPolicy(k)`.
 *
 * The analysis runs in four phases:
 *
 *  1. **Object identification** (`collectConstraints`): Identifies all
 *     memory objects — globals, heap allocations, and stack allocations.
 *  2. **Constraint identification** (`collectConstraints`): Scans every
 *     instruction and emits inclusion constraints of the form `A ⊇ B`
 *     (A can point to everything B can point to).  Handles copies, loads,
 *     stores, and address-taking.
 *  3. **Offline constraint optimisation** (`optimizeConstraints`): Computes
 *     pointer equivalences (nodes with identical points-to sets) and location
 *     equivalences (nodes that always appear together in points-to sets),
 *     then collapses cycles offline to reduce the constraint graph size.
 *  4. **Constraint solving** (`solveConstraints`): Iteratively propagates
 *     inclusion constraints until a fixed point is reached.  Worst-case
 *     complexity is O(N³).
 *
 * ## Function Modelling
 *
 * Functions are modelled as structs with one field per parameter plus a
 * return slot.  Argument `i` of function `F` maps to node index
 * `getNode(F) + CallArgPos + i`; the return value maps to
 * `getNode(F) + CallReturnPos`.  This uniform representation handles
 * indirect calls naturally: an indirect call `(*fp)(a, b)` is modelled as
 * `*(fp + 1) = a`, `*(fp + 2) = b`.
 *
 * ## Context Sensitivity
 *
 * The `ContextPolicy` struct encapsulates the context abstraction:
 *  - `initialCtx` / `globalCtx`: the contexts used for the entry point and
 *    for global initialisers.
 *  - `evolve(ctx, callInst)`: computes the callee context from the caller
 *    context and the call instruction.
 *  - `k`: the call-string depth (0 = context-insensitive).
 *
 * ## Usage
 *
 * ```cpp
 * Andersen aa(M, makeContextPolicy(0));  // context-insensitive
 * std::vector<const llvm::Value *> pts;
 * if (aa.getPointsToSet(ptr, pts)) {
 *   for (auto *obj : pts) { ... }
 * }
 * ```
 */

//
// This file defines an implementation of Andersen's interprocedural alias
// analysis
//
// In pointer analysis terms, this is a subset-based, flow-insensitive
// algorithm. Context sensitivity is configurable: the default is
// context-insensitive, but k-call-site (k-CFA) contexts can be enabled at
// runtime.
//

#pragma once

#include "Alias/InclusionBased/SparrowAA/Constraint.h"
#include "Alias/InclusionBased/SparrowAA/ConstraintSnapshot.h"
#include "Alias/InclusionBased/SparrowAA/NodeFactory.h"
#include "Alias/InclusionBased/SparrowAA/TemplatePtsSet.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/InstrTypes.h> // For CallBase

/**
 * @struct ContextPolicy
 * @brief Encapsulates the context abstraction used by the Andersen analysis.
 *
 * A `ContextPolicy` is a vtable-like struct of function pointers that
 * defines how contexts are created, evolved at call sites, and printed.
 * This design allows the context sensitivity to be selected at runtime
 * without virtual dispatch overhead in the hot constraint-solving loop.
 *
 * - `initialCtx()` — returns the context for the program entry point.
 * - `globalCtx()` — returns the context used for global initialisers.
 * - `evolve(ctx, callInst)` — computes the callee context from the caller
 *   context and the call instruction (implements the k-CFA rule).
 * - `toString(ctx, detailed)` — converts a context to a human-readable string.
 * - `release()` — frees any heap memory owned by the policy (e.g., call-string
 *   interning tables).
 * - `k` — the call-string depth (0 = context-insensitive).
 * - `name` — a short human-readable name for the policy (e.g., "0-CFA").
 */
struct ContextPolicy {
  using Context = AndersNodeFactory::CtxKey;
  using ToStringFn = std::string (*)(Context, bool);
  using EvolveFn = Context (*)(Context, const llvm::Instruction *);

  Context (*initialCtx)(); ///< Returns the initial (entry-point) context.
  Context (*globalCtx)();  ///< Returns the context for global initialisers.
  EvolveFn evolve; ///< Computes callee context from caller context + call site.
  ToStringFn toString; ///< Converts a context to a string for debugging.
  void (*release)();   ///< Releases any resources owned by the policy.
  unsigned k;          ///< Call-string depth (0 = context-insensitive).
  const char *name;    ///< Short name of the policy (e.g., "0-CFA").
};

/**
 * @brief Create a k-call-site context policy.
 * @param kCallSite  The call-string depth.  0 = context-insensitive.
 */
ContextPolicy makeContextPolicy(unsigned kCallSite);

/**
 * @brief Return the context policy selected by the current command-line flags.
 */
ContextPolicy getSelectedAndersenContextPolicy();

/// Forward declaration for the HVN helper used by adaptive CS scoring.
void runHVNAndCapturePELabels(std::vector<AndersConstraint> &constraints,
                              AndersNodeFactory &nodeFactory,
                              llvm::DenseMap<NodeIndex, unsigned> &out);

/**
 * @class Andersen
 * @brief Core Andersen points-to analysis engine.
 *
 * Clients construct an `Andersen` object with a module and a context policy,
 * then query it via `getPointsToSet` / `getPointsToSetInContext`.
 *
 * The analysis is **whole-program**: it processes the entire module in the
 * constructor and stores the resulting points-to graph.  Queries are then
 * answered in O(1) by looking up the pre-computed graph.
 *
 * @note `Andersen` is not an LLVM pass; it is a plain C++ object.  The
 *       `AndersenAAResult` wrapper (in `AndersenAA.h`) adapts it to the
 *       LLVM `AAResultBase` interface.
 */
class Andersen {
public:
  struct Stats {
    size_t numNodes = 0;
    size_t numConstraintsBeforeOpt = 0;
    size_t numConstraintsAfterOpt = 0;
    size_t numSolvingIterations = 0;
    size_t numNonEmptyPtsSets = 0;
    unsigned maxPtsSetSize = 0;
    unsigned avgPtsSetSize = 0;
  };

private:
  /// Factory that allocates and manages all `AndersNode` objects.
  AndersNodeFactory nodeFactory;

  /// All inclusion constraints collected from the module.
  std::vector<AndersConstraint> constraints;

  /// Active points-to set backend captured at construction time.
  PtsSetImpl ptsSetImpl = PtsSetImpl::SPARSE_BITVECTOR;

  /// Dense points-to graph indexed by representative NodeIndex.
  /// Exactly one of `sparsePtsGraph` / `bddPtsGraph` is populated depending on
  /// `ptsSetImpl`, avoiding std::map overhead and RuntimePtsSet type erasure.
  std::vector<AndersPtsSet> sparsePtsGraph;
  std::vector<BDDAndersPtsSet> bddPtsGraph;

  /// Cached metadata for address-taken functions to accelerate indirect-call
  /// target matching without rescanning M or repeatedly calling
  /// Function::hasAddressTaken().
  struct AddrTakenFunctionInfo {
    const llvm::Function *func = nullptr;
    bool retIsPtr = false;
    unsigned numParams = 0;
    uint64_t paramPtrMask = 0;
  };
  std::vector<AddrTakenFunctionInfo> varArgAddrTakenFunctions;
  llvm::DenseMap<unsigned, std::vector<AddrTakenFunctionInfo>>
      fixedArgAddrTakenFunctions;

  Stats stats;

  ContextPolicy ctxPolicy;              ///< The active context policy.
  AndersNodeFactory::CtxKey initialCtx; ///< Context for the entry point.
  AndersNodeFactory::CtxKey globalCtx;  ///< Context for global initialisers.

  /// peLabel map captured from HVN; populated by optimizeConstraints() when
  /// adaptive CS or HVN is enabled. Maps node index → pointer-equivalence label.
  llvm::DenseMap<NodeIndex, unsigned> hvnPELabels;

  /// Per-function context-need score in [0,1]: fraction of pointer formal
  /// parameters that receive pointer-inequivalent actuals from different callers.
  /// Populated by computeContextNeed() after HVN.
  llvm::DenseMap<const llvm::Function *, float> contextNeed;

  /// Key type for the visited-functions set: (function, context) pair.
  struct FunctionContextKey {
    const llvm::Function *func;
    AndersNodeFactory::CtxKey ctx;
  };
  /// DenseSet info for `FunctionContextKey`.
  struct FunctionContextInfo {
    static FunctionContextKey getEmptyKey() {
      return {reinterpret_cast<const llvm::Function *>(-1),
              reinterpret_cast<void *>(0x1)};
    }
    static FunctionContextKey getTombstoneKey() {
      return {reinterpret_cast<const llvm::Function *>(-2),
              reinterpret_cast<void *>(0x2)};
    }
    static unsigned getHashValue(const FunctionContextKey &k) {
      return llvm::hash_combine(k.func, k.ctx);
    }
    static bool isEqual(const FunctionContextKey &lhs,
                        const FunctionContextKey &rhs) {
      return lhs.func == rhs.func && lhs.ctx == rhs.ctx;
    }
  };
  llvm::DenseSet<FunctionContextKey, FunctionContextInfo> visitedFunctions;
  /// Worklist of (function, context) pairs whose instructions still need
  /// processing.  Used instead of recursion to avoid stack overflow on
  /// programs with dense indirect-call graphs.
  std::deque<std::pair<const llvm::Function *, AndersNodeFactory::CtxKey>> pendingFunctions;

  // Three main phases
  void collectConstraints(const llvm::Module &);
  void scheduleFunction(const llvm::Function *, AndersNodeFactory::CtxKey);
  void collectConstraintsForFunction(const llvm::Function *,
                                     AndersNodeFactory::CtxKey);
  void optimizeConstraints();
  void solveConstraints();
  template <typename PtsSetType>
  void solveConstraintsImpl(std::vector<PtsSetType> &ptsGraph);

  // Helper functions for constraint collection
  void collectConstraintsForGlobals(const llvm::Module &,
                                    AndersNodeFactory::CtxKey);
  void collectConstraintsForInstruction(const llvm::Instruction *,
                                        AndersNodeFactory::CtxKey);
  void addGlobalInitializerConstraints(NodeIndex, const llvm::Constant *,
                                       AndersNodeFactory::CtxKey);
  void addConstraintForCall(const llvm::CallBase *cs,
                            AndersNodeFactory::CtxKey callerCtx);
  bool addConstraintForExternalLibrary(const llvm::CallBase *cs,
                                       const llvm::Function *f,
                                       AndersNodeFactory::CtxKey callerCtx);
  void addArgumentConstraintForCall(const llvm::CallBase *cs,
                                    const llvm::Function *f,
                                    AndersNodeFactory::CtxKey callerCtx,
                                    AndersNodeFactory::CtxKey calleeCtx);
  AndersNodeFactory::CtxKey
  evolveContextForCallee(AndersNodeFactory::CtxKey prev,
                         const llvm::Instruction *I,
                         const llvm::Function *callee) const;

  // Helper functions for constraint optimization
  NodeIndex getRefNodeIndex(NodeIndex n) const;
  NodeIndex getAdrNodeIndex(NodeIndex n) const;
  struct FormalActualEdge {
    const llvm::Function *func;
    NodeIndex formal;
    NodeIndex actual;
  };
  void computeContextNeed(const std::vector<FormalActualEdge> &argEdges);

  // For debugging
  void dumpConstraint(const AndersConstraint &) const;
  void dumpConstraints() const;
  void dumpConstraintsPlainVanilla() const;
  void dumpPtsGraphPlainVanilla() const;
  void serializeConstraints(llvm::StringRef outputPath,
                            sparrow_aa::SnapshotPhase phase) const;

public:
  static char ID;

  explicit Andersen(const llvm::Module &,
                    ContextPolicy policy = makeContextPolicy(0));
  ~Andersen();
  bool runOnModule(const llvm::Module &M);

  // Given a llvm pointer v,
  // - Return false if the analysis doesn't know where v points to. In other
  // words, the client must conservatively assume v can points to everything.
  // - Return true otherwise, and the points-to set of v is put into the second
  // argument.
  bool getPointsToSet(const llvm::Value *v,
                      std::vector<const llvm::Value *> &ptsSet) const;
  bool getPointsToSet(const llvm::Value *v, AndersPtsSet &ptsSet) const;
  // Context-sensitive queries (no cross-context union). Return false if the
  // value has no node in the given context; otherwise fill the supplied set.
  bool getPointsToSetInContext(const llvm::Value *v,
                               AndersNodeFactory::CtxKey ctx,
                               std::vector<const llvm::Value *> &ptsSet) const;
  bool getPointsToSetInContext(const llvm::Value *v,
                               AndersNodeFactory::CtxKey ctx,
                               AndersPtsSet &ptsSet) const;

  // Context utilities for clients that want per-context answers.
  AndersNodeFactory::CtxKey getInitialContext() const { return initialCtx; }
  AndersNodeFactory::CtxKey getGlobalContext() const { return globalCtx; }
  AndersNodeFactory::CtxKey evolveContext(AndersNodeFactory::CtxKey prev,
                                          const llvm::Instruction *I) const;
  std::string contextToString(AndersNodeFactory::CtxKey ctx,
                              bool detailed = false) const {
    return ctxPolicy.toString(ctx, detailed);
  }

  /// Return the context-need score for function F (0.0 if not computed or
  /// function has no pointer params). Available after construction when
  /// --andersen-adaptive-cs is set.
  float getContextNeed(const llvm::Function *F) const {
    auto it = contextNeed.find(F);
    return it != contextNeed.end() ? it->second : 0.0f;
  }
  /// Expose the full contextNeed map for iteration (e.g. printing).
  const llvm::DenseMap<const llvm::Function *, float> &
  getContextNeedMap() const {
    return contextNeed;
  }

  // Put all allocation sites (i.e. all memory objects identified by the
  // analysis) into the first arugment
  void
  getAllAllocationSites(std::vector<const llvm::Value *> &allocSites) const;

  const Stats &getStats() const { return stats; }

  friend class AndersenAAResult;
};

