#include "Checker/Pulse/Domain/PulseNonDisjunctiveDomain.h"

#include "Checker/Pulse/Domain/PulseJoin.h"

#include <algorithm>

namespace pulse {

namespace {

static bool addressWasModified(const AbductiveDomain &state,
                               AbstractValue target_raw,
                               uint64_t after_timestamp = 0) {
  AbstractValue canon = state.getCanonical(target_raw);
  if (auto wt = state.getWrittenTo(canon)) {
    if (wt->timestamp > after_timestamp) {
      return true;
    }
  }
  if (state.getUnknownEffect(canon).has_value() ||
      state.getPostAttrs().has(canon, Attribute::Invalid)) {
    return true;
  }
  for (const auto &root_kv : state.getAllocationRoots()) {
    if (state.getCanonical(root_kv.second) == canon) {
      AbstractValue derived = state.getCanonical(root_kv.first);
      if (auto wt = state.getWrittenTo(derived)) {
        if (wt->timestamp > after_timestamp) {
          return true;
        }
      }
      if (state.getUnknownEffect(derived).has_value()) {
        return true;
      }
    }
  }
  return false;
}

} // namespace

void NonDisjunctiveDomain::recordAggregateCopy(const llvm::Instruction *loc,
                                               AbstractValue copied_av,
                                               const std::string &var_name,
                                               const std::string &type_name,
                                               uint64_t copy_timestamp) {
  if (!loc) {
    return;
  }
  auto it = tracked_copies_.find(loc);
  if (it == tracked_copies_.end()) {
    CopyRecord rec;
    rec.location = loc;
    rec.copied_av = copied_av;
    rec.var_name = var_name;
    rec.type_name = type_name;
    rec.copy_timestamp = copy_timestamp;
    rec.modified = false;
    tracked_copies_.emplace(loc, std::move(rec));
  }
}

void NonDisjunctiveDomain::markCopyModified(AbstractValue copied_av) {
  for (auto &kv : tracked_copies_) {
    if (kv.second.copied_av == copied_av) {
      kv.second.modified = true;
    }
  }
}

void NonDisjunctiveDomain::markAddressModified(AbstractValue av,
                                               const AbductiveDomain &state) {
  AbstractValue canon = state.getCanonical(av);
  AbstractValue root = state.getAllocationRoot(canon);
  for (auto &kv : tracked_copies_) {
    AbstractValue copy_c = state.getCanonical(kv.second.copied_av);
    if (copy_c == canon || copy_c == root) {
      kv.second.modified = true;
    }
  }
  for (const auto &kv : candidate_const_ref_params_) {
    AbstractValue param_c = state.getCanonical(kv.second);
    if (param_c == canon || param_c == root) {
      modified_params_.insert(kv.first);
      modified_param_indices_.insert(kv.first->getArgNo());
    }
  }
  for (const auto &kv : all_formal_params_) {
    AbstractValue param_c = state.getCanonical(kv.second);
    if (param_c == canon || param_c == root) {
      modified_param_indices_.insert(kv.first);
    }
  }
}

std::vector<NonDisjunctiveDomain::CopyRecord>
NonDisjunctiveDomain::getUnmodifiedCopies() const {
  std::vector<CopyRecord> out;
  for (const auto &kv : tracked_copies_) {
    if (!kv.second.modified) {
      out.push_back(kv.second);
    }
  }
  return out;
}

void NonDisjunctiveDomain::registerConstRefCandidate(const llvm::Argument *arg,
                                                     AbstractValue av) {
  if (!arg) {
    return;
  }
  candidate_const_ref_params_[arg] = av;
}

void NonDisjunctiveDomain::markParamRead(const llvm::Argument *arg) {
  if (arg) {
    read_params_.insert(arg);
  }
}

void NonDisjunctiveDomain::markParamModified(const llvm::Argument *arg) {
  if (arg) {
    modified_params_.insert(arg);
    modified_param_indices_.insert(arg->getArgNo());
  }
}

std::vector<const llvm::Argument *>
NonDisjunctiveDomain::getConstRefableParams() const {
  std::vector<const llvm::Argument *> result = const_refable_params_;
  if (observed_exits_ == 0) {
    return result;
  }
  for (const auto &kv : candidate_const_ref_params_) {
    const llvm::Argument *arg = kv.first;
    if (modified_params_.count(arg) == 0 && read_params_.count(arg) > 0) {
      if (std::find(result.begin(), result.end(), arg) == result.end()) {
        result.push_back(arg);
      }
    }
  }
  return result;
}

void NonDisjunctiveDomain::addState(const AbductiveDomain &state) {
  observed_exits_++;

  for (auto &kv : tracked_copies_) {
    CopyRecord &rec = kv.second;
    if (rec.modified) {
      continue;
    }
    if (addressWasModified(state, rec.copied_av, rec.copy_timestamp)) {
      rec.modified = true;
    }
  }

  for (const auto &kv : all_formal_params_) {
    if (modified_param_indices_.count(kv.first) == 0 &&
        addressWasModified(state, kv.second, 0)) {
      modified_param_indices_.insert(kv.first);
    }
  }

  for (const auto &kv : candidate_const_ref_params_) {
    const llvm::Argument *arg = kv.first;
    if (modified_params_.count(arg) > 0) {
      continue;
    }
    AbstractValue param_av = state.getCanonical(kv.second);
    if (addressWasModified(state, param_av, 0)) {
      modified_params_.insert(arg);
      modified_param_indices_.insert(arg->getArgNo());
      continue;
    }
    if (state.getPreMustBeInitialized().count(param_av) > 0) {
      read_params_.insert(arg);
    } else {
      auto pre_it = state.getPreHeap().getEdges().find(param_av);
      if (pre_it != state.getPreHeap().getEdges().end() &&
          !pre_it->second.empty()) {
        read_params_.insert(arg);
      }
    }
  }

  if (!summary_) {
    summary_ = std::make_unique<AbductiveDomain>(state.clone());
    return;
  }

  if (auto joined_opt = PulseJoin::joinAbductive(*summary_, state)) {
    summary_ = std::make_unique<AbductiveDomain>(std::move(*joined_opt));
    return;
  }

  AbductiveDomain intersection = summary_->clone();
  const AbductiveDomain &existing = *summary_;
  {
    Stack new_stack;
    for (const auto &kv : existing.getPostStack().getMap()) {
      const Address *rhs_addr = state.getPostStack().find(kv.first);
      if (rhs_addr && existing.getCanonical(kv.second.addr) ==
                          state.getCanonical(rhs_addr->addr)) {
        new_stack.add(kv.first, kv.second);
      }
    }
    intersection.getPostStack() = std::move(new_stack);
  }
  intersection.canonicalize();
  summary_ = std::make_unique<AbductiveDomain>(std::move(intersection));
}

void NonDisjunctiveDomain::join(const NonDisjunctiveDomain &other) {
  for (const auto &kv : other.tracked_copies_) {
    auto it = tracked_copies_.find(kv.first);
    if (it == tracked_copies_.end()) {
      tracked_copies_.emplace(kv.first, kv.second);
    } else {
      it->second.modified = it->second.modified || kv.second.modified;
    }
  }
  for (const auto &kv : other.candidate_const_ref_params_) {
    candidate_const_ref_params_.emplace(kv.first, kv.second);
  }
  for (const auto &kv : other.all_formal_params_) {
    all_formal_params_.emplace(kv.first, kv.second);
  }
  modified_params_.insert(other.modified_params_.begin(),
                          other.modified_params_.end());
  modified_param_indices_.insert(other.modified_param_indices_.begin(),
                                 other.modified_param_indices_.end());
  read_params_.insert(other.read_params_.begin(), other.read_params_.end());
  observed_exits_ += other.observed_exits_;

  for (const auto *S : other.copied_stores_)
    copied_stores_.push_back(S);
  for (const auto *A : other.const_refable_params_)
    const_refable_params_.push_back(A);

  if (other.summary_) {
    addState(*other.summary_);
  }
}

NonDisjunctiveSummary NonDisjunctiveDomain::createSummary() const {
  return NonDisjunctiveSummary(
      summary_ ? std::make_unique<AbductiveDomain>(summary_->clone()) : nullptr,
      modified_param_indices_);
}

NonDisjunctiveSummary
NonDisjunctiveSummary::join(const NonDisjunctiveSummary &s1,
                            const NonDisjunctiveSummary &s2) {
  std::set<unsigned> merged_mods = s1.modified_param_indices_;
  merged_mods.insert(s2.modified_param_indices_.begin(),
                     s2.modified_param_indices_.end());
  if (!s1.summary_ && !s2.summary_) {
    return NonDisjunctiveSummary(nullptr, std::move(merged_mods));
  }
  if (!s1.summary_) {
    return NonDisjunctiveSummary(
        std::make_unique<AbductiveDomain>(s2.summary_->clone()),
        std::move(merged_mods));
  }
  if (!s2.summary_) {
    return NonDisjunctiveSummary(
        std::make_unique<AbductiveDomain>(s1.summary_->clone()),
        std::move(merged_mods));
  }
  NonDisjunctiveDomain domain;
  domain.addState(*s1.getSummary());
  domain.addState(*s2.getSummary());
  auto out = domain.createSummary();
  return NonDisjunctiveSummary(
      out.getSummary() ? std::make_unique<AbductiveDomain>(
                             out.getSummary()->clone())
                       : nullptr,
      std::move(merged_mods));
}

} // namespace pulse
