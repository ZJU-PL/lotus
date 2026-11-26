/** @file PulseTransitiveInfo.h @brief Transitive callee information for Pulse interprocedural analysis. */
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/Error.h>

namespace llvm {
class Function;
class Instruction;
} // namespace llvm

namespace pulse {

/**
 * Policy rule for Pulse's TransitiveAccess checker (Infer
 * PulseTransitiveAccessChecker.ml): flags when any function matching
 * `source_functions` transitively calls a function matching
 * `forbidden_callees`.
 */
struct TransitiveAccessPolicy {
  std::string name;
  std::vector<std::string> source_functions;
  std::vector<std::string> forbidden_callees;
  std::string description;
};

llvm::Expected<std::vector<TransitiveAccessPolicy>>
loadTransitiveAccessPoliciesFromJson(llvm::StringRef json_text);

/**
 * TransitiveInfo: tracks interprocedural information
 * Records transitive accesses and call resolutions
 */
class TransitiveInfo {
public:
  struct AccessInfo {
    const llvm::Instruction *location;
    const llvm::Function *function;

    AccessInfo(const llvm::Instruction *loc, const llvm::Function *func)
        : location(loc), function(func) {}
  };

  enum class CallResolution {
    Resolved,       // Call was resolved to a specific function
    Unresolved,     // Call could not be resolved
    MultipleTargets // Call has multiple possible targets
  };

  struct CallInfo {
    const llvm::Instruction *call_site;
    const llvm::Function *caller;
    CallResolution resolution;
    std::vector<const llvm::Function *> targets;

    CallInfo(const llvm::Instruction *cs, const llvm::Function *c,
             CallResolution r)
        : call_site(cs), caller(c), resolution(r) {}
  };

private:
  static constexpr size_t kMaxTransitiveEntries = 256;
  std::vector<AccessInfo> transitive_accesses_;
  std::vector<CallInfo> call_resolutions_;

public:
  TransitiveInfo() = default;

  /**
   * Record a transitive access (deduplicated and bounded).
   */
  void recordTransitiveAccess(const llvm::Instruction *loc,
                              const llvm::Function *func);

  /**
   * Record call resolution (deduplicated and bounded).
   */
  void recordCallResolution(
      const llvm::Instruction *call_site, const llvm::Function *caller,
      CallResolution resolution,
      const std::vector<const llvm::Function *> &targets = {});

  const std::vector<AccessInfo> &getTransitiveAccesses() const {
    return transitive_accesses_;
  }
  const std::vector<CallInfo> &getCallResolutions() const {
    return call_resolutions_;
  }

  TransitiveInfo clone() const {
    TransitiveInfo cloned;
    cloned.transitive_accesses_ = transitive_accesses_;
    cloned.call_resolutions_ = call_resolutions_;
    return cloned;
  }

  /**
   * Merge two transitive info objects
   */
  static TransitiveInfo merge(const TransitiveInfo &t1,
                              const TransitiveInfo &t2);
};

} // namespace pulse

