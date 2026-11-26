#include "Checker/Pulse/Interproc/PulseSummary.h"

#include "Checker/Pulse/Domain/PulseNonDisjunctiveDomain.h"

#include <optional>

namespace pulse {

SummaryEntry::SummaryEntry(std::unique_ptr<AbductiveDomain> pre,
                           PulseFormula pre_formula,
                           std::unique_ptr<AbductiveDomain> post,
                           PulseFormula post_formula,
                           std::optional<AbstractValue> ret_val,
                           std::optional<LatentIssueSummary> latent_issue,
                           bool is_exception_raised,
                           std::optional<AbstractValue> exception_value,
                           std::string exception_type)
    : pre_(std::shared_ptr<const AbductiveDomain>(std::move(pre))),
      pre_formula_(std::move(pre_formula)),
      post_(std::shared_ptr<const AbductiveDomain>(std::move(post))),
      post_formula_(std::move(post_formula)), return_value_(ret_val),
      latent_issue_(std::move(latent_issue)),
      is_exception_raised_(is_exception_raised),
      exception_value_(exception_value),
      exception_type_(std::move(exception_type)) {}

SummaryEntry::SummaryEntry(std::shared_ptr<const AbductiveDomain> shared_domain,
                           PulseFormula unified_formula,
                           std::optional<AbstractValue> ret_val,
                           std::optional<LatentIssueSummary> latent_issue,
                           bool is_exception_raised,
                           std::optional<AbstractValue> exception_value,
                           std::string exception_type)
    : pre_(shared_domain), pre_formula_(std::move(unified_formula)),
      post_(std::move(shared_domain)), post_formula_shared_with_pre_(true),
      return_value_(ret_val), latent_issue_(std::move(latent_issue)),
      is_exception_raised_(is_exception_raised),
      exception_value_(exception_value),
      exception_type_(std::move(exception_type)) {}

SummaryEntry SummaryEntry::clone() const {
  std::optional<LatentIssueSummary> latent;
  if (latent_issue_) {
    LatentIssueSummary copied;
    copied.diagnostic = latent_issue_->diagnostic;
    copied.address = latent_issue_->address;
    copied.trace = latent_issue_->trace.clone();
    copied.calling_context = latent_issue_->calling_context;
    copied.is_latent_invalid_access = latent_issue_->is_latent_invalid_access;
    copied.must_be_valid_reason = latent_issue_->must_be_valid_reason;
    latent = std::move(copied);
  }
  if (pre_ == post_ && post_formula_shared_with_pre_) {
    return SummaryEntry(pre_, pre_formula_.clone(), return_value_,
                        std::move(latent), is_exception_raised_,
                        exception_value_, exception_type_);
  }
  return SummaryEntry(
      pre_ ? std::make_unique<AbductiveDomain>(pre_->clone()) : nullptr,
      pre_formula_.clone(),
      post_ ? std::make_unique<AbductiveDomain>(post_->clone()) : nullptr,
      getPostFormula().clone(), return_value_, std::move(latent),
      is_exception_raised_, exception_value_, exception_type_);
}

PulseSummary::PulseSummary(const llvm::Function *F) : function_(F) {}

PulseSummary::PulseSummary(const llvm::Function *F,
                           std::unique_ptr<AbductiveDomain> pre,
                           PulseFormula pre_formula,
                           std::unique_ptr<AbductiveDomain> post,
                           PulseFormula post_formula,
                           std::optional<AbstractValue> ret_val)
    : function_(F) {
  pre_post_list_.emplace_back(std::move(pre), std::move(pre_formula),
                              std::move(post), std::move(post_formula),
                              ret_val);
}

std::optional<AbstractValue>
PulseSummary::getFormalAV(const llvm::Value *formal) const {
  auto it = formal_to_av_.find(formal);
  if (it != formal_to_av_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void PulseSummary::setFormalAV(const llvm::Value *formal, AbstractValue av) {
  formal_to_av_[formal] = av;
}

void PulseSummary::addPrePost(SummaryEntry entry) {
  pre_post_list_.push_back(std::move(entry));
}

const AbductiveDomain *PulseSummary::getPre() const {
  if (pre_post_list_.empty()) {
    return nullptr;
  }
  return pre_post_list_[0].getPre();
}

const AbductiveDomain *PulseSummary::getPost() const {
  if (pre_post_list_.empty()) {
    return nullptr;
  }
  return pre_post_list_[0].getPost();
}

const PulseFormula &PulseSummary::getPreFormula() const {
  static PulseFormula empty;
  if (pre_post_list_.empty()) {
    return empty;
  }
  return pre_post_list_[0].getPreFormula();
}

const PulseFormula &PulseSummary::getPostFormula() const {
  static PulseFormula empty;
  if (pre_post_list_.empty()) {
    return empty;
  }
  return pre_post_list_[0].getPostFormula();
}

std::optional<AbstractValue> PulseSummary::getReturnValue() const {
  if (pre_post_list_.empty()) {
    return std::nullopt;
  }
  return pre_post_list_[0].getReturnValue();
}

PulseSummary PulseSummary::join(const PulseSummary &s1,
                                const PulseSummary &s2) {
  PulseSummary joined(s1.function_);

  for (const auto &entry : s1.pre_post_list_) {
    joined.pre_post_list_.push_back(entry.clone());
  }
  for (const auto &entry : s2.pre_post_list_) {
    joined.pre_post_list_.push_back(entry.clone());
  }

  joined.non_disj_ = NonDisjunctiveSummary::join(s1.non_disj_, s2.non_disj_);
  joined.transitive_info_ =
      TransitiveInfo::merge(s1.transitive_info_, s2.transitive_info_);

  joined.formal_to_av_ = s1.formal_to_av_;
  for (const auto &kv : s2.formal_to_av_) {
    if (joined.formal_to_av_.find(kv.first) == joined.formal_to_av_.end()) {
      joined.formal_to_av_[kv.first] = kv.second;
    }
  }

  return joined;
}

void SummaryManager::storeSummary(const llvm::Function *F,
                                  PulseSummary summary) {
  summaries_.erase(F);
  summaries_.emplace(F, std::move(summary));
}

const PulseSummary *SummaryManager::getSummary(const llvm::Function *F) const {
  auto it = summaries_.find(F);
  if (it != summaries_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool SummaryManager::hasSummary(const llvm::Function *F) const {
  return summaries_.find(F) != summaries_.end();
}

void SummaryManager::clear() {
  summaries_.clear();
  skipped_calls_.clear();
}

} // namespace pulse
