/** @file PulseTaintConfig.h @brief Taint configuration for Pulse taint analysis domain. */
#pragma once

#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Error.h>

namespace pulse {

/**
 * TaintKind: category of taint source/sink (Infer's TaintConfig.Kind).
 */
class TaintKind {
private:
  std::string name_;
  bool is_data_flow_only_;

public:
  explicit TaintKind(const std::string &name, bool is_data_flow_only = false)
      : name_(name), is_data_flow_only_(is_data_flow_only) {}

  const std::string &getName() const { return name_; }
  bool isDataFlowOnly() const { return is_data_flow_only_; }

  bool operator<(const TaintKind &other) const { return name_ < other.name_; }
  bool operator==(const TaintKind &other) const { return name_ == other.name_; }
  bool operator!=(const TaintKind &other) const { return name_ != other.name_; }

  static TaintKind UserInput() { return TaintKind("UserInput"); }
  static TaintKind Network() { return TaintKind("Network"); }
  static TaintKind FileSystem() { return TaintKind("FileSystem"); }
  static TaintKind Environment() { return TaintKind("Environment"); }
  static TaintKind Sensitive() { return TaintKind("Sensitive"); }
  static TaintKind Unknown() { return TaintKind("Unknown"); }
};

/**
 * ProcedureMatcher: matches procedures for taint sources/sinks/sanitizers.
 */
class ProcedureMatcher {
public:
  enum class Type {
    ProcedureName,       // Exact name match
    ExactNames,          // O(1) set of exact procedure names
    ProcedureNameRegex,  // Regex match on raw or demangled name
    ClassNameRegex,      // Regex match on demangled class name
    ClassAndMethodNames, // Match demangled class and method names
    BuiltinName          // Builtin/intrinsic function name
  };

private:
  Type type_;
  std::string name_;
  std::unordered_set<std::string> exact_names_;
  std::regex name_regex_;
  std::vector<std::string> class_names_;
  std::vector<std::string> method_names_;
  std::vector<std::string> exclude_in_;
  std::vector<std::string> exclude_names_;

public:
  ProcedureMatcher(Type t, const std::string &name) : type_(t), name_(name) {
    if (t == Type::ProcedureNameRegex || t == Type::ClassNameRegex) {
      name_regex_ = std::regex(name);
    }
  }

  static ProcedureMatcher
  forExactNames(std::initializer_list<const char *> names) {
    ProcedureMatcher m(Type::ExactNames, "");
    for (const char *n : names) {
      m.exact_names_.emplace(n);
    }
    return m;
  }

  static ProcedureMatcher
  forClassAndMethods(std::vector<std::string> classes,
                     std::vector<std::string> methods) {
    ProcedureMatcher m(Type::ClassAndMethodNames, "");
    m.class_names_ = std::move(classes);
    m.method_names_ = std::move(methods);
    return m;
  }

  void addExcludeName(std::string name) {
    exclude_names_.push_back(std::move(name));
  }
  void addExcludeIn(std::string scope) {
    exclude_in_.push_back(std::move(scope));
  }

  bool matches(const llvm::Function *func) const;
  bool matchesName(const std::string &func_name,
                   bool is_intrinsic = false) const;

  Type getType() const { return type_; }
  const std::string &getName() const { return name_; }
};

/**
 * ProcedureTarget: specifies which values of a call are tainted/checked.
 */
enum class ProcedureTarget {
  ReturnValue,
  AllArguments,
  ArgumentPositions,
  InstanceReference,
  AllArgumentsButPositions
};

/**
 * ProcedureUnit: defines a taint source/sink/sanitizer/propagator rule.
 */
struct ProcedureUnit {
  ProcedureMatcher matcher;
  std::vector<TaintKind> kinds;
  ProcedureTarget target;
  std::vector<int> argument_positions;
  std::optional<unsigned> field_index;

  ProcedureUnit(const ProcedureMatcher &m, const std::vector<TaintKind> &k,
                ProcedureTarget t, std::vector<int> positions = {},
                std::optional<unsigned> field_idx = std::nullopt)
      : matcher(m), kinds(k), target(t),
        argument_positions(std::move(positions)), field_index(field_idx) {}

  bool appliesToReturn() const {
    return target == ProcedureTarget::ReturnValue;
  }

  bool appliesToArgument(unsigned arg_idx) const {
    switch (target) {
    case ProcedureTarget::ReturnValue:
      return false;
    case ProcedureTarget::AllArguments:
      return true;
    case ProcedureTarget::InstanceReference:
      return arg_idx == 0;
    case ProcedureTarget::ArgumentPositions:
      for (int pos : argument_positions) {
        if (pos >= 0 && static_cast<unsigned>(pos) == arg_idx)
          return true;
      }
      return false;
    case ProcedureTarget::AllArgumentsButPositions:
      for (int pos : argument_positions) {
        if (pos >= 0 && static_cast<unsigned>(pos) == arg_idx)
          return false;
      }
      return true;
    }
    return false;
  }
};

/**
 * SinkPolicy: defines when a taint flow should be reported.
 */
struct SinkPolicy {
  std::vector<TaintKind> source_kinds;
  std::vector<TaintKind> sanitizer_kinds;
  std::string description;
  int policy_id;
  std::string privacy_effect;
  std::string report_as_issue_type;
  std::string report_as_category;

  SinkPolicy(int id, const std::string &desc)
      : description(desc), policy_id(id) {}
};

/**
 * TaintConfig: unified configuration for Pulse taint analysis.
 */
class TaintConfig {
private:
  std::vector<ProcedureUnit> source_procedures_;
  std::vector<ProcedureUnit> source_blocks_;
  std::vector<ProcedureUnit> sink_procedures_;
  std::vector<ProcedureUnit> sanitizer_procedures_;
  std::vector<ProcedureUnit> propagator_procedures_;
  std::map<TaintKind, std::vector<SinkPolicy>> sink_policies_;
  std::map<std::string, std::vector<TaintKind>> allocation_sources_;

  // Memoized match caches keyed by "(I:|F:)<func_name>" storing unit indices,
  // plus fast Function* pointer caches to avoid std::string construction on
  // hot call-site queries.
  mutable std::unordered_map<std::string, std::vector<size_t>> source_cache_;
  mutable std::unordered_map<std::string, std::vector<size_t>>
      source_block_cache_;
  mutable std::unordered_map<std::string, std::vector<size_t>> sink_cache_;
  mutable std::unordered_map<std::string, std::vector<size_t>> sanitizer_cache_;
  mutable std::unordered_map<std::string, std::vector<size_t>>
      propagator_cache_;
  mutable std::unordered_map<const llvm::Function *, std::vector<size_t>>
      source_fn_cache_;
  mutable std::unordered_map<const llvm::Function *, std::vector<size_t>>
      source_block_fn_cache_;
  mutable std::unordered_map<const llvm::Function *, std::vector<size_t>>
      sink_fn_cache_;
  mutable std::unordered_map<const llvm::Function *, std::vector<size_t>>
      sanitizer_fn_cache_;
  mutable std::unordered_map<const llvm::Function *, std::vector<size_t>>
      propagator_fn_cache_;

  void clearMatchCaches() {
    source_cache_.clear();
    source_block_cache_.clear();
    sink_cache_.clear();
    sanitizer_cache_.clear();
    propagator_cache_.clear();
    source_fn_cache_.clear();
    source_block_fn_cache_.clear();
    sink_fn_cache_.clear();
    sanitizer_fn_cache_.clear();
    propagator_fn_cache_.clear();
  }

  static TaintConfig *default_config_;

public:
  TaintConfig() = default;

  void addSourceProcedure(const ProcedureUnit &unit) {
    source_procedures_.push_back(unit);
    source_cache_.clear();
    source_fn_cache_.clear();
  }
  void addSourceBlock(const ProcedureUnit &unit) {
    source_blocks_.push_back(unit);
    source_block_cache_.clear();
    source_block_fn_cache_.clear();
  }
  void addSinkProcedure(const ProcedureUnit &unit) {
    sink_procedures_.push_back(unit);
    sink_cache_.clear();
    sink_fn_cache_.clear();
  }
  void addSanitizerProcedure(const ProcedureUnit &unit) {
    sanitizer_procedures_.push_back(unit);
    sanitizer_cache_.clear();
    sanitizer_fn_cache_.clear();
  }
  void addPropagatorProcedure(const ProcedureUnit &unit) {
    propagator_procedures_.push_back(unit);
    propagator_cache_.clear();
    propagator_fn_cache_.clear();
  }
  void addSinkPolicy(const TaintKind &sink_kind, const SinkPolicy &policy) {
    sink_policies_[sink_kind].push_back(policy);
  }
  void addAllocationSource(const std::string &class_name,
                           const std::vector<TaintKind> &kinds) {
    allocation_sources_[class_name] = kinds;
  }

  // Unit-level matchers (respect ProcedureTarget & ArgumentPositions).
  std::vector<const ProcedureUnit *>
  matchSources(const llvm::Function *func) const;
  std::vector<const ProcedureUnit *>
  matchSourcesByName(const std::string &func_name) const;
  std::vector<const ProcedureUnit *>
  matchSourceBlocks(const llvm::Function *func) const;
  std::vector<const ProcedureUnit *>
  matchSinks(const llvm::Function *func) const;
  std::vector<const ProcedureUnit *>
  matchSinksByName(const std::string &func_name) const;
  std::vector<const ProcedureUnit *>
  matchSanitizers(const llvm::Function *func) const;
  std::vector<const ProcedureUnit *>
  matchSanitizersByName(const std::string &func_name) const;
  std::vector<const ProcedureUnit *>
  matchPropagators(const llvm::Function *func) const;
  std::vector<const ProcedureUnit *>
  matchPropagatorsByName(const std::string &func_name) const;

  // Convenience queries
  std::vector<TaintKind> isSource(const llvm::Function *func) const;
  bool isSink(const llvm::Function *func,
              std::vector<TaintKind> &sink_kinds) const;
  std::vector<TaintKind> isSanitizer(const llvm::Function *func) const;
  bool isPropagator(const llvm::Function *func) const;

  const std::vector<SinkPolicy> &
  getSinkPolicies(const TaintKind &sink_kind) const;
  std::vector<TaintKind>
  getAllocationSourceKinds(const std::string &class_name) const;
  std::vector<TaintKind>
  matchAllocationSourceForName(const std::string &name) const;

  static const TaintConfig &getDefault();
  static void initializeDefault();

  /**
   * Parse custom taint rules from JSON text (optionally extending default).
   */
  static llvm::Expected<TaintConfig>
  loadFromJson(llvm::StringRef json_text, bool extend_default = true);
};

} // namespace pulse
