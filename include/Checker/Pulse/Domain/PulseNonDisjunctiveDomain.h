/** @file PulseNonDisjunctiveDomain.h @brief Non-disjunctive abstract domain for Pulse analysis. */
#pragma once

#include "Checker/Pulse/Domain/PulseDomain.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace llvm {
class Function;
class Argument;
class StoreInst;
} // namespace llvm

namespace pulse {

class NonDisjunctiveSummary;

/**
 * NonDisjunctiveDomain: must-style / over-approximated non-disjunctive state
 * aggregated across all explored disjuncts (Infer's PulseNonDisjunctiveDomain).
 */
class NonDisjunctiveDomain {
public:
  struct CopyRecord {
    const llvm::Instruction *location{nullptr};
    AbstractValue copied_av;
    std::string var_name;
    std::string type_name;
    uint64_t copy_timestamp{0};
    bool modified{false};
  };

private:
  std::unique_ptr<AbductiveDomain> summary_;
  std::vector<const llvm::StoreInst *> copied_stores_;
  std::vector<const llvm::Argument *> const_refable_params_;

  std::map<const llvm::Instruction *, CopyRecord> tracked_copies_;
  std::map<const llvm::Argument *, AbstractValue> candidate_const_ref_params_;
  std::map<unsigned, AbstractValue> all_formal_params_;
  std::set<const llvm::Argument *> modified_params_;
  std::set<unsigned> modified_param_indices_;
  std::set<const llvm::Argument *> read_params_;
  unsigned observed_exits_{0};

public:
  NonDisjunctiveDomain() = default;

  void addState(const AbductiveDomain &state);

  const AbductiveDomain *getSummary() const { return summary_.get(); }
  AbductiveDomain *getSummary() { return summary_.get(); }

  bool isEmpty() const {
    return summary_ == nullptr && tracked_copies_.empty() &&
           candidate_const_ref_params_.empty() &&
           modified_param_indices_.empty();
  }

  void clear() {
    summary_.reset();
    copied_stores_.clear();
    const_refable_params_.clear();
    tracked_copies_.clear();
    candidate_const_ref_params_.clear();
    all_formal_params_.clear();
    modified_params_.clear();
    modified_param_indices_.clear();
    read_params_.clear();
    observed_exits_ = 0;
  }

  void join(const NonDisjunctiveDomain &other);

  void recordCopy(const llvm::StoreInst *SI) { copied_stores_.push_back(SI); }
  void recordConstRefableParam(const llvm::Argument *A) {
    const_refable_params_.push_back(A);
  }
  const std::vector<const llvm::StoreInst *> &getCopiedStores() const {
    return copied_stores_;
  }

  void recordAggregateCopy(const llvm::Instruction *loc,
                           AbstractValue copied_av, const std::string &var_name,
                           const std::string &type_name,
                           uint64_t copy_timestamp);
  void markCopyModified(AbstractValue copied_av);
  void markAddressModified(AbstractValue av, const AbductiveDomain &state);
  std::vector<CopyRecord> getUnmodifiedCopies() const;

  void registerFormalParam(unsigned arg_idx, AbstractValue av) {
    all_formal_params_[arg_idx] = av;
  }
  void registerConstRefCandidate(const llvm::Argument *arg, AbstractValue av);
  void markParamRead(const llvm::Argument *arg);
  void markParamModified(const llvm::Argument *arg);
  std::vector<const llvm::Argument *> getConstRefableParams() const;
  const std::set<unsigned> &getModifiedParamIndices() const {
    return modified_param_indices_;
  }

  NonDisjunctiveSummary createSummary() const;
};

/**
 * NonDisjunctiveSummary: summary component exported per procedure.
 */
class NonDisjunctiveSummary {
private:
  std::unique_ptr<AbductiveDomain> summary_;
  std::set<unsigned> modified_param_indices_;

public:
  NonDisjunctiveSummary() = default;

  explicit NonDisjunctiveSummary(std::unique_ptr<AbductiveDomain> summary,
                                 std::set<unsigned> modified_params = {})
      : summary_(std::move(summary)),
        modified_param_indices_(std::move(modified_params)) {}

  NonDisjunctiveSummary(NonDisjunctiveSummary &&) = default;
  NonDisjunctiveSummary &operator=(NonDisjunctiveSummary &&) = default;

  NonDisjunctiveSummary(const NonDisjunctiveSummary &other)
      : summary_(other.summary_ ? std::make_unique<AbductiveDomain>(
                                      other.summary_->clone())
                                : nullptr),
        modified_param_indices_(other.modified_param_indices_) {}

  NonDisjunctiveSummary &operator=(const NonDisjunctiveSummary &other) {
    if (this != &other) {
      summary_ =
          other.summary_
              ? std::make_unique<AbductiveDomain>(other.summary_->clone())
              : nullptr;
      modified_param_indices_ = other.modified_param_indices_;
    }
    return *this;
  }

  const AbductiveDomain *getSummary() const { return summary_.get(); }
  AbductiveDomain *getSummary() { return summary_.get(); }

  const std::set<unsigned> &getModifiedParamIndices() const {
    return modified_param_indices_;
  }
  bool isParamModified(unsigned arg_idx) const {
    return modified_param_indices_.count(arg_idx) > 0;
  }

  bool isEmpty() const {
    return summary_ == nullptr && modified_param_indices_.empty();
  }

  NonDisjunctiveSummary clone() const {
    return NonDisjunctiveSummary(
        summary_ ? std::make_unique<AbductiveDomain>(summary_->clone())
                 : nullptr,
        modified_param_indices_);
  }

  static NonDisjunctiveSummary join(const NonDisjunctiveSummary &s1,
                                    const NonDisjunctiveSummary &s2);
};

} // namespace pulse
