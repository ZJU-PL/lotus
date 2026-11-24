#pragma once

#include "Dataflow/IFDS/Analyses/AbstractMemoryLocation.h"
#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <algorithm>
#include <string>
#include <unordered_set>

namespace ifds {

struct ExtendedTaintValue {
  enum Kind { Bottom, Clean, Sanitized, Tainted, Top } kind;

  ExtendedTaintValue() : kind(Bottom) {}
  explicit ExtendedTaintValue(Kind k) : kind(k) {}

  static ExtendedTaintValue bottom() { return ExtendedTaintValue(Bottom); }
  static ExtendedTaintValue clean() { return ExtendedTaintValue(Clean); }
  static ExtendedTaintValue sanitized() {
    return ExtendedTaintValue(Sanitized);
  }
  static ExtendedTaintValue tainted() { return ExtendedTaintValue(Tainted); }
  static ExtendedTaintValue top() { return ExtendedTaintValue(Top); }

  bool operator==(const ExtendedTaintValue &other) const {
    return kind == other.kind;
  }
};

class IDEExtendedTaintAnalysis
    : public DefaultNoAliasIDEProblem<const llvm::Value *, ExtendedTaintValue> {
public:
  using Fact = const llvm::Value *;
  using Value = ExtendedTaintValue;

  IDEExtendedTaintAnalysis();
  explicit IDEExtendedTaintAnalysis(const TaintConfig &model);

  const llvm::Value *sparse_fact_value(const Fact &fact) const override {
    return fact;
  }
  bool is_identity_flow(const llvm::Instruction *inst,
                        const llvm::Instruction *succ,
                        const Fact &fact) const override;
  bool is_identity_edge(const llvm::Instruction *, const llvm::Instruction *,
                        const Fact &) const override {
    return true;
  }

  Fact zero_fact() const override { return nullptr; }
  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ, const Fact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const Fact &fact) override;
  FactSet return_flow(const llvm::CallBase *call,
                      const llvm::Instruction *exit_inst,
                      const llvm::Instruction *return_site,
                      const llvm::Function *callee, const Fact &exit_fact,
                      const Fact &call_fact) override;
  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees,
                              const Fact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override {
    return this->lift_ifds_initial_seeds(module, bottom_value());
  }

  Value top_value() const override { return Value::top(); }
  Value bottom_value() const override { return Value::bottom(); }
  Value join(const Value &v1, const Value &v2) const override;

  EdgeFunction normal_edge_function(const llvm::Instruction *stmt,
                                    const llvm::Instruction *succ,
                                    const Fact &src_fact,
                                    const Fact &tgt_fact) override;
  EdgeFunction call_edge_function(const llvm::CallBase *call,
                                  const llvm::Function *callee,
                                  const Fact &src_fact,
                                  const Fact &tgt_fact) override;
  EdgeFunction return_edge_function(const llvm::CallBase *call,
                                    const llvm::Function *callee,
                                    const llvm::Instruction *exit_inst,
                                    const llvm::Instruction *return_site,
                                    const Fact &exit_fact,
                                    const Fact &ret_fact) override;
  EdgeFunction call_to_return_edge_function(
      const llvm::CallBase *call, const llvm::Instruction *return_site,
      llvm::ArrayRef<const llvm::Function *> callees, const Fact &src_fact,
      const Fact &tgt_fact) override;

  FactSet summary_flow(const llvm::CallBase *call, const llvm::Function *callee,
                       const Fact &fact) override;
  EdgeFunction summary_edge_function(const llvm::CallBase *call,
                                     const llvm::Function *callee,
                                     const llvm::Instruction *return_site,
                                     const Fact &src_fact,
                                     const Fact &tgt_fact) override;

private:
  std::shared_ptr<const TaintConfig> m_model;
  const TaintConfig &model() const;

  bool is_source_function(const llvm::Function *callee) const;
  bool is_sanitizer_function(const llvm::Function *callee) const;
};

/// Alias-aware, field-sensitive variant of IDEExtendedTaintAnalysis.
///
/// Facts are interned abstract memory locations rather than raw SSA values.
/// The location factory records constant GEP offsets up to `path_bound`, and
/// flow functions use Lotus's optional alias wrapper for load/store and
/// sanitizer updates. The original IDEExtendedTaintAnalysis remains available
/// for clients that intentionally want the lightweight SSA-only domain.
class IDEAliasAwareExtendedTaintAnalysis
    : public DefaultAliasAwareIDEProblem<AbstractMemoryLocation,
                                         ExtendedTaintValue> {
public:
  using Fact = AbstractMemoryLocation;
  using Value = ExtendedTaintValue;

  explicit IDEAliasAwareExtendedTaintAnalysis(unsigned path_bound = 3,
                                              bool strong_updates = true);
  IDEAliasAwareExtendedTaintAnalysis(const TaintConfig &model,
                                     unsigned path_bound = 3,
                                     bool strong_updates = true);

  Fact zero_fact() const override { return m_factory.zero(); }
  const llvm::Value *sparse_fact_value(const Fact &fact) const override {
    return fact.is_zero() ? nullptr : fact.base();
  }
  bool is_identity_flow(const llvm::Instruction *inst,
                        const llvm::Instruction *succ,
                        const Fact &fact) const override;
  bool is_identity_edge(const llvm::Instruction *, const llvm::Instruction *,
                        const Fact &) const override;

  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const Fact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const Fact &fact) override;
  FactSet return_flow(const llvm::CallBase *call,
                      const llvm::Instruction *exit_inst,
                      const llvm::Instruction *return_site,
                      const llvm::Function *callee, const Fact &exit_fact,
                      const Fact &call_fact) override;
  FactSet call_to_return_flow(
      const llvm::CallBase *call, const llvm::Instruction *return_site,
      llvm::ArrayRef<const llvm::Function *> callees,
      const Fact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override;

  Value top_value() const override { return Value::top(); }
  Value bottom_value() const override { return Value::bottom(); }
  Value join(const Value &v1, const Value &v2) const override;

  EdgeFunction normal_edge_function(const llvm::Instruction *stmt,
                                    const llvm::Instruction *succ,
                                    const Fact &src_fact,
                                    const Fact &tgt_fact) override;
  EdgeFunction call_edge_function(const llvm::CallBase *call,
                                  const llvm::Function *callee,
                                  const Fact &src_fact,
                                  const Fact &tgt_fact) override;
  EdgeFunction return_edge_function(const llvm::CallBase *call,
                                    const llvm::Function *callee,
                                    const llvm::Instruction *exit_inst,
                                    const llvm::Instruction *return_site,
                                    const Fact &exit_fact,
                                    const Fact &ret_fact) override;
  EdgeFunction call_to_return_edge_function(
      const llvm::CallBase *call, const llvm::Instruction *return_site,
      llvm::ArrayRef<const llvm::Function *> callees, const Fact &src_fact,
      const Fact &tgt_fact) override;
  FactSet summary_flow(const llvm::CallBase *call, const llvm::Function *callee,
                       const Fact &fact) override;
  EdgeFunction summary_edge_function(const llvm::CallBase *call,
                                     const llvm::Function *callee,
                                     const llvm::Instruction *return_site,
                                     const Fact &src_fact,
                                     const Fact &tgt_fact) override;

  unsigned path_bound() const noexcept { return m_path_bound; }
  void set_path_bound(unsigned bound) noexcept {
    m_path_bound = std::max(1u, bound);
  }
  bool strong_updates() const noexcept { return m_strong_updates; }
  void set_strong_updates(bool enabled) noexcept { m_strong_updates = enabled; }
  const AbstractMemoryLocationFactory &location_factory() const noexcept {
    return m_factory;
  }
  AbstractMemoryLocationFactory &location_factory() noexcept { return m_factory; }
  /// Build the interned memory fact used by this analysis for an LLVM value.
  /// The module data layout is installed when solving the first module.
  Fact memory_location(const llvm::Value *value) const {
    return make_fact(value);
  }

private:
  std::shared_ptr<const TaintConfig> m_model;
  mutable AbstractMemoryLocationFactory m_factory;
  unsigned m_path_bound;
  bool m_strong_updates;

  const TaintConfig &model() const;
  void configure_module(const llvm::Module &module);
  Fact make_fact(const llvm::Value *value) const;
  bool is_source_function(const llvm::Function *callee) const;
  bool is_sanitizer_function(const llvm::Function *callee) const;
  const FunctionTaintConfig *function_config(
      const llvm::Function *callee) const;
  void add_configured_source_facts(const llvm::CallBase *call,
                                   const llvm::Function *callee,
                                   FactSet &out) const;
  void add_configured_pipe_facts(const llvm::CallBase *call,
                                 const llvm::Function *callee,
                                 const Fact &fact, FactSet &out) const;
  bool fact_matches_value(const Fact &fact, const llvm::Value *value) const;
  bool fact_may_alias_value(const Fact &fact, const llvm::Value *value) const;
};

// Descriptive aliases used by clients migrating from PhASAR's XTaint name.
using IDEExtendedTaintMemoryAnalysis = IDEAliasAwareExtendedTaintAnalysis;
using IDEXTaintAnalysis = IDEAliasAwareExtendedTaintAnalysis;

} // namespace ifds
