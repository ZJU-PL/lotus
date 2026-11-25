#pragma once

#include "Alias/InclusionBased/BootstrapAA/Engine.h"

namespace llvm {
class CallBase;
class Function;
class Instruction;
class Module;
class Value;
} // namespace llvm

namespace lotus {
namespace bootstrap {

// LLVM 14 adapter. The module must outlive this object and remain unchanged.
// Results describe executions starting at entry (default: main), after global
// initialization. This is a sequential, closed-world analysis, not an LLVM AA
// pass: it requires a program point and, optionally, a matched call-site
// context.
class BootstrapAA {
public:
  using CallContext = std::vector<const llvm::CallBase *>;
  explicit BootstrapAA(const llvm::Module &module,
                       const llvm::Function *entry = nullptr,
                       Options options = {});
  ~BootstrapAA();
  BootstrapAA(BootstrapAA &&) noexcept;
  BootstrapAA &operator=(BootstrapAA &&) noexcept;
  BootstrapAA(const BootstrapAA &) = delete;
  BootstrapAA &operator=(const BootstrapAA &) = delete;

  QueryResult pointsTo(const llvm::Value &value, const llvm::Instruction &site,
                       const CallContext &context = {},
                       Point point = Point::Before,
                       ContextMatch match = ContextMatch::Full);
  QueryResult pointsToAllContexts(const llvm::Value &value,
                                  const llvm::Instruction &site,
                                  Point point = Point::Before);
  struct LLVMAliasResult {
    std::vector<const llvm::Value *> aliases;
    // Aliased addressable memory pointer object IDs (e.g., globals/allocas/
    // flattened struct pointer fields holding an aliased pointer value).
    // Map back to LLVM storage via allocationSite(id) and objectInfo(id).
    std::vector<Id> object_aliases;
    bool may_alias_all = false;
    QueryStatus status = QueryStatus::Unreachable;
    bool reachable() const { return status != QueryStatus::Unreachable; }
  };
  struct LLVMCallTargets {
    std::vector<const llvm::Function *> functions;
    bool may_call_unknown = false;
    QueryStatus status = QueryStatus::Complete;
    bool reachable() const { return status != QueryStatus::Unreachable; }
  };
  struct LLVMReversePointsToResult {
    std::vector<const llvm::Value *> values;
    std::vector<Id> memory_objects;
    bool may_be_pointed_by_unknown = false;
  };
  LLVMAliasResult aliases(const llvm::Value &value,
                          const llvm::Instruction &site,
                          const CallContext &context = {},
                          Point point = Point::Before,
                          ContextMatch match = ContextMatch::Full);
  LLVMAliasResult aliasesAllContexts(const llvm::Value &value,
                                     const llvm::Instruction &site,
                                     Point point = Point::Before);
  QueryResult objectPointsTo(Id object, const llvm::Instruction &site,
                             const CallContext &context = {},
                             Point point = Point::Before,
                             ContextMatch match = ContextMatch::Full);
  QueryResult objectPointsToAllContexts(Id object,
                                        const llvm::Instruction &site,
                                        Point point = Point::Before);
  LLVMAliasResult objectAliases(Id object, const llvm::Instruction &site,
                                const CallContext &context = {},
                                Point point = Point::Before,
                                ContextMatch match = ContextMatch::Full);
  LLVMAliasResult objectAliasesAllContexts(Id object,
                                           const llvm::Instruction &site,
                                           Point point = Point::Before);
  void precomputeAll();
  // Application-driven selective cluster exploration (§1, p. 250): solve only
  // the clusters containing any pointer in `targets` (e.g., lock pointers).
  void precomputeForValues(const std::vector<const llvm::Value *> &targets);
  std::vector<std::size_t> clustersFor(const llvm::Value &value) const;
  std::vector<std::vector<const llvm::Value *>> clusters() const;
  bool mayAlias(const llvm::Value &lhs, const llvm::Value &rhs,
                const llvm::Instruction &site, const CallContext &context = {},
                ContextMatch match = ContextMatch::Full);
  // Cross-site may-alias helpers (all-contexts and per-side contexts).
  bool mayAliasAcrossSites(const llvm::Value &lhs,
                           const llvm::Instruction &lhs_site,
                           const llvm::Value &rhs,
                           const llvm::Instruction &rhs_site,
                           Point lhs_point = Point::Before,
                           Point rhs_point = Point::Before);
  bool mayAliasAcrossSites(const llvm::Value &lhs,
                           const llvm::Instruction &lhs_site,
                           const CallContext &lhs_context,
                           const llvm::Value &rhs,
                           const llvm::Instruction &rhs_site,
                           const CallContext &rhs_context,
                           Point lhs_point = Point::Before,
                           Point rhs_point = Point::Before,
                           ContextMatch lhs_match = ContextMatch::Full,
                           ContextMatch rhs_match = ContextMatch::Full);
  // Four-state byte-range-aware alias query (size == 0 means unknown/whole
  // object access).
  AliasKind queryAlias(const llvm::Value &lhs, std::uint64_t lhs_size,
                       const llvm::Instruction &lhs_site,
                       const llvm::Value &rhs, std::uint64_t rhs_size,
                       const llvm::Instruction &rhs_site,
                       Point lhs_point = Point::Before,
                       Point rhs_point = Point::Before);
  AliasKind queryAlias(const llvm::Value &lhs, std::uint64_t lhs_size,
                       const llvm::Instruction &lhs_site,
                       const CallContext &lhs_context, const llvm::Value &rhs,
                       std::uint64_t rhs_size,
                       const llvm::Instruction &rhs_site,
                       const CallContext &rhs_context,
                       Point lhs_point = Point::Before,
                       Point rhs_point = Point::Before,
                       ContextMatch lhs_match = ContextMatch::Full,
                       ContextMatch rhs_match = ContextMatch::Full);
  // Non-throwing site-free conservative alias convenience query (returns true
  // for untracked / non-pointer / unreachable values instead of throwing).
  bool mayAliasGlobal(const llvm::Value &lhs, const llvm::Value &rhs);
  bool mustAlias(const llvm::Value &lhs, const llvm::Value &rhs,
                 const llvm::Instruction &site, const CallContext &context = {},
                 ContextMatch match = ContextMatch::Full);
  bool mayNull(const llvm::Value &value, const llvm::Instruction &site,
               Point point = Point::Before);
  bool mayNull(const llvm::Value &value, const llvm::Instruction &site,
               const CallContext &context, Point point = Point::Before,
               ContextMatch match = ContextMatch::Full);
  // Derived call-graph, Mod/Ref, and reverse points-to results.
  LLVMCallTargets callTargets(const llvm::CallBase &call);
  LLVMCallTargets callTargets(const llvm::CallBase &call,
                              const CallContext &context,
                              ContextMatch match = ContextMatch::Full);
  std::vector<const llvm::CallBase *>
  callersOf(const llvm::Function &function) const;
  bool isRecursiveFunction(const llvm::Function &function) const;
  ModRefResult functionModRef(const llvm::Function &function);
  ModRefResult callModRef(const llvm::CallBase &call);
  ModRefResult callModRef(const llvm::CallBase &call,
                          const CallContext &context,
                          ContextMatch match = ContextMatch::Full);
  LLVMReversePointsToResult reversePointsTo(Id object);
  bool hasValue(const llvm::Value &value) const;
  bool hasSite(const llvm::Instruction &site) const;

  // UNKNOWN and NULL_OBJECT have no LLVM allocation site. A function object
  // maps to its Function, a global to its GlobalVariable, and storage to its
  // allocating AllocaInst/CallBase. Flattened struct field objects map back to
  // their parent allocation site; use objectInfo(object).offset for the field
  // byte offset. INVALID means the value is not an allocation site.
  const llvm::Value *allocationSite(Id object) const;
  Id objectId(const llvm::Value &allocation) const;
  // Look up a flattened field object at a byte offset within allocation
  // (returns INVALID if no such flattened field object exists).
  Id fieldObjectId(const llvm::Value &allocation,
                   std::uint64_t byte_offset) const;
  const Object &objectInfo(Id object) const;
  const Statistics &statistics() const;
  const SteensgaardHierarchy &hierarchy() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace bootstrap
} // namespace lotus
