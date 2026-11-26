/** @file PulseSummary.h @brief Procedure summary storage for Pulse interprocedural analysis. */
#pragma once

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Domain/PulseNonDisjunctiveDomain.h"
#include "Checker/Pulse/Interproc/PulseTransitiveInfo.h"

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <llvm/IR/Function.h>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace pulse {

/**
 * SummaryEntry: represents a single pre/post pair in a disjunctive summary.
 */
class SummaryEntry {
public:
  struct LatentIssueSummary {
    OperationResult diagnostic{OperationResult::InvalidAccess};
    AbstractValue address;
    Trace trace;
    std::vector<std::pair<const llvm::Function *, const llvm::Instruction *>>
        calling_context;
    bool is_latent_invalid_access{false};
    std::optional<InvalidationKind> must_be_valid_reason;
  };

private:
  std::unique_ptr<AbductiveDomain> pre_;
  PulseFormula pre_formula_;
  std::unique_ptr<AbductiveDomain> post_;
  PulseFormula post_formula_;
  std::optional<AbstractValue> return_value_;

  std::optional<LatentIssueSummary> latent_issue_;
  bool is_exception_raised_{false};
  std::optional<AbstractValue> exception_value_;
  std::string exception_type_;

public:
  SummaryEntry(std::unique_ptr<AbductiveDomain> pre, PulseFormula pre_formula,
               std::unique_ptr<AbductiveDomain> post, PulseFormula post_formula,
               std::optional<AbstractValue> ret_val,
               std::optional<LatentIssueSummary> latent_issue = std::nullopt,
               bool is_exception_raised = false,
               std::optional<AbstractValue> exception_value = std::nullopt,
               std::string exception_type = "");

  SummaryEntry(SummaryEntry &&) = default;
  SummaryEntry &operator=(SummaryEntry &&) = default;

  SummaryEntry(const SummaryEntry &) = delete;
  SummaryEntry &operator=(const SummaryEntry &) = delete;

  const AbductiveDomain *getPre() const { return pre_.get(); }
  const AbductiveDomain *getPost() const { return post_.get(); }
  const PulseFormula &getPreFormula() const { return pre_formula_; }
  const PulseFormula &getPostFormula() const { return post_formula_; }
  std::optional<AbstractValue> getReturnValue() const { return return_value_; }
  const std::optional<LatentIssueSummary> &getLatentIssue() const {
    return latent_issue_;
  }
  bool isExceptionRaised() const { return is_exception_raised_; }
  std::optional<AbstractValue> getExceptionValue() const {
    return exception_value_;
  }
  const std::string &getExceptionType() const { return exception_type_; }

  SummaryEntry clone() const;
};

/**
 * PulseSummary: represents a function summary for interprocedural analysis.
 * Contains disjunctive pre/post pairs, non-disjunctive summary, and transitive
 * info.
 */
class PulseSummary {
private:
  const llvm::Function *function_;
  std::vector<SummaryEntry> pre_post_list_;
  NonDisjunctiveSummary non_disj_;
  TransitiveInfo transitive_info_;
  std::map<const llvm::Value *, AbstractValue> formal_to_av_;

public:
  explicit PulseSummary(const llvm::Function *F);

  PulseSummary(const llvm::Function *F, std::unique_ptr<AbductiveDomain> pre,
               PulseFormula pre_formula, std::unique_ptr<AbductiveDomain> post,
               PulseFormula post_formula,
               std::optional<AbstractValue> ret_val);

  const llvm::Function *getFunction() const { return function_; }

  const std::vector<SummaryEntry> &getPrePostList() const {
    return pre_post_list_;
  }
  void addPrePost(SummaryEntry entry);

  PulseSummary(PulseSummary &&) = default;
  PulseSummary &operator=(PulseSummary &&) = default;

  PulseSummary(const PulseSummary &) = delete;
  PulseSummary &operator=(const PulseSummary &) = delete;

  const NonDisjunctiveSummary &getNonDisj() const { return non_disj_; }
  NonDisjunctiveSummary &getNonDisj() { return non_disj_; }
  void setNonDisj(NonDisjunctiveSummary non_disj) {
    non_disj_ = std::move(non_disj);
  }

  const TransitiveInfo &getTransitiveInfo() const { return transitive_info_; }
  TransitiveInfo &getTransitiveInfo() { return transitive_info_; }
  void setTransitiveInfo(TransitiveInfo info) {
    transitive_info_ = std::move(info);
  }

  // Legacy accessors (for backward compatibility)
  const AbductiveDomain *getPre() const;
  const AbductiveDomain *getPost() const;
  const PulseFormula &getPreFormula() const;
  const PulseFormula &getPostFormula() const;
  std::optional<AbstractValue> getReturnValue() const;

  std::optional<AbstractValue> getFormalAV(const llvm::Value *formal) const;
  void setFormalAV(const llvm::Value *formal, AbstractValue av);

  bool isValid() const {
    return !pre_post_list_.empty() || !non_disj_.isEmpty();
  }

  static PulseSummary join(const PulseSummary &s1, const PulseSummary &s2);
};

/**
 * SkippedCalls: tracks function calls for which no summary was found.
 */
class SkippedCalls {
private:
  struct SkippedCall {
    const llvm::Function *function;
    const llvm::Instruction *call_site;
    const llvm::Function *caller;

    SkippedCall(const llvm::Function *f, const llvm::Instruction *cs,
                const llvm::Function *c)
        : function(f), call_site(cs), caller(c) {}
  };

  std::vector<SkippedCall> skipped_calls_;

public:
  SkippedCalls() = default;

  void addSkippedCall(const llvm::Function *func,
                      const llvm::Instruction *call_site,
                      const llvm::Function *caller) {
    skipped_calls_.emplace_back(func, call_site, caller);
  }

  const std::vector<SkippedCall> &getSkippedCalls() const {
    return skipped_calls_;
  }
  bool isEmpty() const { return skipped_calls_.empty(); }
  size_t size() const { return skipped_calls_.size(); }

  void clear() { skipped_calls_.clear(); }

  SkippedCalls clone() const {
    SkippedCalls cloned;
    cloned.skipped_calls_ = skipped_calls_;
    return cloned;
  }
};

/**
 * SummaryManager: manages function summaries for interprocedural analysis.
 */
class SummaryManager {
private:
  std::map<const llvm::Function *, PulseSummary> summaries_;
  SkippedCalls skipped_calls_;

public:
  void storeSummary(const llvm::Function *F, PulseSummary summary);
  const PulseSummary *getSummary(const llvm::Function *F) const;
  bool hasSummary(const llvm::Function *F) const;

  void recordSkippedCall(const llvm::Function *func,
                         const llvm::Instruction *call_site,
                         const llvm::Function *caller) {
    skipped_calls_.addSkippedCall(func, call_site, caller);
  }

  const SkippedCalls &getSkippedCalls() const { return skipped_calls_; }
  void clear();
};

} // namespace pulse
