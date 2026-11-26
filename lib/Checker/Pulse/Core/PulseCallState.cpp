#include "Checker/Pulse/Core/PulseCallState.h"

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseAbductiveDomain.h"

namespace pulse {

void CallState::canonicalizeMaps() {
  if (!astate_) {
    return;
  }

  Substitution normalized_subst;
  for (const auto &kv : subst_.getMap()) {
    normalized_subst.add(kv.first, astate_->getCanonical(kv.second));
  }
  subst_ = std::move(normalized_subst);

  std::map<AbstractValue, std::pair<AbstractValue, LazyHeapPath>>
      normalized_rev_subst;
  for (const auto &kv : rev_subst_) {
    normalized_rev_subst.emplace(astate_->getCanonical(kv.first), kv.second);
  }
  rev_subst_ = std::move(normalized_rev_subst);

  std::map<AbstractValue, std::vector<LazyHeapPath>> normalized_alias_paths;
  for (const auto &kv : alias_paths_by_caller_) {
    auto &dst = normalized_alias_paths[astate_->getCanonical(kv.first)];
    dst.insert(dst.end(), kv.second.begin(), kv.second.end());
  }
  alias_paths_by_caller_ = std::move(normalized_alias_paths);

  if (first_error_) {
    first_error_ = astate_->getCanonical(*first_error_);
  }
}

IncorporateResult CallState::incorporateNewEqs(const NewEqs &new_eqs) {
  if (!astate_) {
    return IncorporateResult::ok();
  }
  IncorporateResult res = astate_->incorporateNewEqs(new_eqs);
  if (!res.isOk()) {
    return res;
  }
  canonicalizeMaps();
  return res;
}

bool CallState::incorporateNewEqs(const PulseFormula &new_eqs) {
  if (astate_) {
    PulseFormula merged =
        PulseFormula::merge(astate_->getPathFormula(), new_eqs);
    if (!merged.isConsistent()) {
      return false;
    }
    NewEqs discovered = merged.takeNewEqs();
    astate_->setPathFormula(std::make_unique<PulseFormula>(std::move(merged)));
    auto res = astate_->incorporateNewEqs(discovered);
    if (!res.isOk()) {
      return false;
    }
    astate_->canonicalize();
  }
  canonicalizeMaps();
  return true;
}

} // namespace pulse
