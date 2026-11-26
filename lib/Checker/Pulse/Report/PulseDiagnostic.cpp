#include "Checker/Pulse/Report/PulseDiagnostic.h"

#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

namespace pulse {

// --- AccessToInvalidAddress ---

size_t AccessToInvalidAddress::getHash() const {
  // Hash based on location, issue type, and invalidation kind
  // We don't hash the full trace to allow merging similar issues with different
  // paths
  size_t h = std::hash<const llvm::Instruction *>{}(location_);
  h = h ^ (std::hash<std::string>{}(issue_type_) << 1);
  h = h ^ (std::hash<int>{}(static_cast<int>(invalidation_kind_)) << 2);
  return h;
}

bool AccessToInvalidAddress::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const AccessToInvalidAddress *>(&other);
  if (!o)
    return false;
  return location_ == o->location_ && issue_type_ == o->issue_type_ &&
         invalidation_kind_ == o->invalidation_kind_;
}

// --- ResourceLeak ---

std::string ResourceLeak::getMessage() const {
  return "Resource leak detected";
}

std::string ResourceLeak::getDescription() const {
  return resource_name_ + " acquired here is never released";
}

size_t ResourceLeak::getHash() const {
  size_t h = std::hash<const llvm::Instruction *>{}(allocation_site_);
  h = h ^ (std::hash<std::string>{}(resource_name_) << 1);
  return h;
}

bool ResourceLeak::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const ResourceLeak *>(&other);
  if (!o)
    return false;
  return allocation_site_ == o->allocation_site_ &&
         resource_name_ == o->resource_name_;
}

// --- TaintFlow ---

std::string TaintFlow::getMessage() const {
  return "Tainted data flows to sensitive sink";
}

std::string TaintFlow::getDescription() const {
  return "Data from " + source_kind_ + " flows to " + sink_kind_;
}

size_t TaintFlow::getHash() const {
  size_t h = std::hash<const llvm::Instruction *>{}(location_);
  h = h ^ (std::hash<std::string>{}(source_kind_) << 1);
  h = h ^ (std::hash<std::string>{}(sink_kind_) << 2);
  return h;
}

bool TaintFlow::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const TaintFlow *>(&other);
  if (!o)
    return false;
  return location_ == o->location_ && source_kind_ == o->source_kind_ &&
         sink_kind_ == o->sink_kind_;
}

// --- UnnecessaryCopy ---

std::string UnnecessaryCopy::getMessage() const {
  return "Unnecessary copy of " + variable_name_;
}

std::string UnnecessaryCopy::getDescription() const {
  return "Variable " + variable_name_ + " of type " + type_name_ +
         " is copied but the copy is not modified";
}

std::string UnnecessaryCopy::getSuggestion() const {
  return "Use a const reference (const &) to avoid the copy.";
}

size_t UnnecessaryCopy::getHash() const {
  size_t h = std::hash<const llvm::Instruction *>{}(location_);
  h = h ^ (std::hash<std::string>{}(variable_name_) << 1);
  return h;
}

bool UnnecessaryCopy::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const UnnecessaryCopy *>(&other);
  if (!o)
    return false;
  return location_ == o->location_ && variable_name_ == o->variable_name_;
}

// --- StackVariableAddressEscape ---

size_t StackVariableAddressEscape::getHash() const {
  size_t h = std::hash<const llvm::Instruction *>{}(location_);
  h = h ^ (std::hash<unsigned>{}(address_.getId()) << 1);
  return h;
}

bool StackVariableAddressEscape::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const StackVariableAddressEscape *>(&other);
  if (!o)
    return false;
  return location_ == o->location_ && address_.getId() == o->address_.getId();
}

// --- InvalidFree ---

size_t InvalidFree::getHash() const {
  size_t h = std::hash<const llvm::Instruction *>{}(location_);
  h = h ^ (std::hash<unsigned>{}(address_.getId()) << 1);
  return h;
}

bool InvalidFree::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const InvalidFree *>(&other);
  if (!o)
    return false;
  return location_ == o->location_ && address_.getId() == o->address_.getId();
}

// --- MemoryLeak ---

std::string MemoryLeak::getMessage() const { return "Memory leak detected"; }

std::string MemoryLeak::getDescription() const {
  return "Memory allocated via " + allocator_name_ +
         " becomes unreachable without being freed";
}

size_t MemoryLeak::getHash() const {
  size_t h = std::hash<const llvm::Instruction *>{}(allocation_site_);
  h = h ^ (std::hash<std::string>{}(allocator_name_) << 1);
  return h;
}

bool MemoryLeak::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const MemoryLeak *>(&other);
  if (!o)
    return false;
  return allocation_site_ == o->allocation_site_ &&
         allocator_name_ == o->allocator_name_;
}

// --- ConstRefableParam ---

std::string ConstRefableParam::getMessage() const {
  return "Parameter " + param_name_ + " can be passed by const reference";
}

std::string ConstRefableParam::getDescription() const {
  return "Parameter " + param_name_ + " of type " + type_name_ +
         " is copied by value and never modified in the function";
}

std::string ConstRefableParam::getSuggestion() const {
  return "Pass " + param_name_ + " by const reference (const &) to avoid "
         "an unnecessary copy.";
}

size_t ConstRefableParam::getHash() const {
  size_t h = std::hash<const llvm::Instruction *>{}(location_);
  h = h ^ (std::hash<std::string>{}(param_name_) << 1);
  return h;
}

bool ConstRefableParam::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const ConstRefableParam *>(&other);
  if (!o)
    return false;
  return location_ == o->location_ && param_name_ == o->param_name_;
}

// --- MutualRecursionCycle ---

std::string MutualRecursionCycle::getMessage() const {
  return "Mutual recursion cycle detected";
}

std::string MutualRecursionCycle::getDescription() const {
  return "Unbounded recursive call cycle with unchanged arguments: " +
         cycle_description_;
}

std::string MutualRecursionCycle::getSuggestion() const {
  return "Ensure recursive calls make progress toward a terminating base case.";
}

size_t MutualRecursionCycle::getHash() const {
  size_t h = std::hash<const llvm::Instruction *>{}(location_);
  h = h ^ (std::hash<std::string>{}(cycle_description_) << 1);
  return h;
}

bool MutualRecursionCycle::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const MutualRecursionCycle *>(&other);
  if (!o)
    return false;
  return location_ == o->location_ &&
         cycle_description_ == o->cycle_description_;
}

// --- RetainCycle ---

std::string RetainCycle::getMessage() const { return "Retain cycle detected"; }

std::string RetainCycle::getDescription() const {
  return "Strong reference cycle prevents deallocation: " + cycle_description_;
}

std::string RetainCycle::getSuggestion() const {
  return "Break the ownership cycle using std::weak_ptr or explicit reset().";
}

size_t RetainCycle::getHash() const {
  size_t h = std::hash<const llvm::Instruction *>{}(location_);
  h = h ^ (std::hash<std::string>{}(cycle_description_) << 1);
  return h;
}

bool RetainCycle::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const RetainCycle *>(&other);
  if (!o)
    return false;
  return location_ == o->location_ &&
         cycle_description_ == o->cycle_description_;
}

// --- InfiniteLoop ---

std::string InfiniteLoop::getMessage() const {
  return "Infinite loop detected";
}

std::string InfiniteLoop::getDescription() const {
  return description_.empty() ? "Loop header repeats identical state with no "
                                "progress toward termination"
                              : description_;
}

std::string InfiniteLoop::getSuggestion() const {
  return "Ensure loop variables make progress toward the exit condition.";
}

size_t InfiniteLoop::getHash() const {
  return std::hash<const llvm::Instruction *>{}(location_) ^ 0x9e3779b9u;
}

bool InfiniteLoop::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const InfiniteLoop *>(&other);
  if (!o)
    return false;
  return location_ == o->location_;
}

// --- TransitiveAccess ---

std::string TransitiveAccess::getMessage() const {
  return "Forbidden transitive call (" + policy_name_ + ")";
}

std::string TransitiveAccess::getDescription() const { return detail_; }

std::string TransitiveAccess::getSuggestion() const {
  return "Avoid transitively calling restricted operations from this context.";
}

size_t TransitiveAccess::getHash() const {
  size_t h = std::hash<const llvm::Instruction *>{}(location_);
  h = h ^ (std::hash<std::string>{}(policy_name_) << 1);
  h = h ^ (std::hash<std::string>{}(detail_) << 2);
  return h;
}

bool TransitiveAccess::equals(const Diagnostic &other) const {
  const auto *o = dynamic_cast<const TransitiveAccess *>(&other);
  if (!o)
    return false;
  return location_ == o->location_ && policy_name_ == o->policy_name_ &&
         detail_ == o->detail_;
}

} // namespace pulse
