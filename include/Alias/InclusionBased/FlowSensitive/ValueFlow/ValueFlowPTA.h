#pragma once

#include "Alias/InclusionBased/FlowSensitive/ValueFlow/ValueFlowGraph.h"

#include <memory>
#include <set>
#include <vector>

namespace llvm {
class Function;
class Module;
class StoreInst;
class Value;
} // namespace llvm

namespace lotus::alias {

/// Context-insensitive, field-insensitive, flow-sensitive points-to analysis
/// implementing Li, Cifuentes, and Keynes (ESEC/FSE 2011).
///
/// This analysis builds its own per-function VFG directly from LLVM IR without
/// consuming a precomputed SVFG or running a flow-insensitive pre-analysis.
/// Aggregates and heap allocation sites are monolithic summary objects, as in
/// the paper. Null and unknown have explicit, distinct ObjectIDs.
///
/// The module must outlive the analysis, and must not be mutated between
/// analyze() and queries. analyze() reconstructs all state on every invocation.
class ValueFlowPTA {
public:
  using ObjectID = vfg::ObjectID;
  using NodeID = vfg::NodeID;
  using PointsToSet = vfg::PointsToSet;
  using PointedToBySet = std::set<const llvm::Value *>;
  using PointedToByStoreSet = std::set<const llvm::StoreInst *>;

  struct Config {
    bool enableStrongUpdates = true;
    // Match Section 4.1 of Li et al. (2011): model pointer-typed loads/stores
    // (plus pointer-containing aggregates and ptrtoint-derived writes) rather
    // than treating arbitrary integer/float stores as unknown pointer writes.
    bool pointerOnlyMemory = true;
    // Use Lotus's AliasSpecManager for library functions (allocators,
    // deallocators, memcpy/memset/strcpy, pure/no-effect functions, etc.).
    bool useExternalSpecs = true;
    // Synthesize a startup/teardown wrapper for llvm.global_ctors and
    // llvm.global_dtors in priority order instead of rejecting modules that
    // contain global lifecycle lists.
    bool lowerGlobalLifecycle = true;
    // Empty: use a defined main; otherwise all externally visible definitions,
    // or all definitions if there are no externally visible ones.
    std::vector<const llvm::Function *> entryPoints;
  };
  struct Statistics {
    vfg::Solver::Statistics solver;
    std::size_t callGraphIterations = 0;
    std::size_t resolvedIndirectTargets = 0;
    std::size_t conservativeExternalCalls = 0;
    std::size_t specModeledCalls = 0;
    std::size_t globalCtors = 0;
    std::size_t globalDtors = 0;
    std::size_t functions = 0;
    std::size_t callSites = 0;
    std::size_t objects = 0;
    std::size_t valueNodes = 0;
    std::size_t controlBlocks = 0;
  };

  explicit ValueFlowPTA(const llvm::Module &module);
  ValueFlowPTA(const llvm::Module &module, Config config);
  ~ValueFlowPTA();
  ValueFlowPTA(const ValueFlowPTA &) = delete;
  ValueFlowPTA &operator=(const ValueFlowPTA &) = delete;

  void analyze();
  // Empty for an untracked/non-pointer value; mayAlias remains conservative
  // for untracked values. Query results are invalidated by the next analyze().
  const PointsToSet &getPointsTo(const llvm::Value *value) const;
  const PointedToBySet &getPointedToBy(ObjectID object) const;
  PointedToByStoreSet getPointedToByStores(ObjectID object) const;
  bool mayAlias(const llvm::Value *left, const llvm::Value *right) const;
  ObjectID getObjectId(const llvm::Value *allocation) const;
  // Null for the two distinguished objects or an invalid ObjectID.
  const llvm::Value *getObjectValue(ObjectID object) const;
  ObjectID getNullObjectId() const;
  ObjectID getUnknownObjectId() const;
  const Statistics &getStatistics() const;

  // VFG inspection API for downstream value-flow clients (Section 1 & 7).
  const vfg::Program &getProgram() const;
  const vfg::Solver &getSolver() const;
  NodeID getValueNode(const llvm::Value *value) const;
  NodeID getStoreNode(const llvm::StoreInst *store) const;
  const llvm::Value *getNodeValue(NodeID node) const;
  vfg::NodeSet getVFGSuccessors(NodeID node) const;
  vfg::NodeSet getVFGIndirectSuccessors(NodeID node) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::alias
