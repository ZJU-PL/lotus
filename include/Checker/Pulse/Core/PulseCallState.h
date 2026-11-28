/** @file PulseCallState.h @brief Call state tracking for Pulse interprocedural analysis. */
#pragma once

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Core/PulseMemory.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Core/PulseValueHistory.h"
#include "Checker/Pulse/Domain/PulseAbductiveDomain.h"
#include "Checker/Pulse/Interproc/PulseSpecialization.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Argument.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Value.h>

namespace pulse {

/**
 * CallState: state tracked during interprocedural summary application.
 * Aligned with Infer's call_state structure in PulseInterproc.ml.
 */
class CallState {
public:
  /**
   * LazyHeapPath: callee-rooted heap path built incrementally during
   * pre-materialization, convertible to a concrete Specialization::HeapPath.
   */
  class LazyHeapPath {
  public:
    enum class Kind { Supported, Unsupported };

  private:
    Kind kind_ = Kind::Unsupported;
    int param_index_{-1};
    const llvm::Value *pvar_ = nullptr;
    std::string root_name_;
    std::vector<Access> steps_; // Root-to-leaf order

  public:
    LazyHeapPath() = default;

    static LazyHeapPath fromFormal(unsigned param_idx,
                                   const llvm::Value *formal) {
      LazyHeapPath path;
      path.kind_ = Kind::Supported;
      path.param_index_ = static_cast<int>(param_idx);
      path.pvar_ = formal;
      path.root_name_ =
          (formal && formal->hasName())
              ? formal->getName().str()
              : ("arg" + std::to_string(param_idx));
      return path;
    }

    static LazyHeapPath fromGlobal(const llvm::Value *gv) {
      LazyHeapPath path;
      if (!gv) {
        return unsupported();
      }
      path.kind_ = Kind::Supported;
      path.param_index_ = -1;
      path.pvar_ = gv;
      path.root_name_ = gv->hasName() ? gv->getName().str() : "<global>";
      return path;
    }

    static LazyHeapPath fromPvar(const llvm::Value *pvar) {
      if (!pvar) {
        return unsupported();
      }
      if (const auto *arg = llvm::dyn_cast<llvm::Argument>(pvar)) {
        return fromFormal(arg->getArgNo(), arg);
      }
      if (llvm::isa<llvm::GlobalValue>(pvar)) {
        return fromGlobal(pvar);
      }
      LazyHeapPath path;
      path.kind_ = Kind::Supported;
      path.pvar_ = pvar;
      path.root_name_ = pvar->hasName() ? pvar->getName().str() : "<pvar>";
      return path;
    }

    static LazyHeapPath unsupported() {
      LazyHeapPath path;
      path.kind_ = Kind::Unsupported;
      return path;
    }

    LazyHeapPath push(Access access) const {
      if (kind_ == Kind::Unsupported) {
        return unsupported();
      }
      LazyHeapPath new_path = *this;
      new_path.steps_.push_back(access);
      return new_path;
    }

    std::optional<HeapPath> force() const {
      if (kind_ == Kind::Unsupported) {
        return std::nullopt;
      }
      HeapPath hp;
      if (param_index_ >= 0) {
        hp = HeapPath::fromFormal(static_cast<unsigned>(param_index_),
                                  root_name_);
      } else if (pvar_) {
        hp = HeapPath::fromGlobal(pvar_, root_name_);
      } else {
        return std::nullopt;
      }
      for (const Access &acc : steps_) {
        switch (acc.kind) {
        case AccessKind::Dereference:
          hp.pushDeref();
          break;
        case AccessKind::Field:
          hp.pushField(acc.field_idx, acc.field_byte_offset);
          break;
        case AccessKind::ArrayIndex:
          if (acc.const_index.has_value()) {
            hp.pushConstArrayIndex(*acc.const_index, acc.stride_bytes);
          } else {
            hp.pushArrayIndex(acc.index, acc.stride_bytes);
          }
          break;
        }
      }
      return hp;
    }

    bool isSupported() const { return kind_ == Kind::Supported; }
  };

  struct CalleeIndexToVisit {
    AbstractValue addr_pre_dest; // Destination address in callee pre
    ValueHistory pre_hist;       // History in callee pre
    Access access_callee;        // ArrayIndex access in callee
    std::pair<AbstractValue, ValueHistory>
        addr_hist_caller;        // Base address/history in caller
    LazyHeapPath base_path;

    CalleeIndexToVisit(AbstractValue addr_pre, const ValueHistory &hist,
                       Access acc,
                       const std::pair<AbstractValue, ValueHistory> &caller,
                       LazyHeapPath bp = LazyHeapPath::unsupported())
        : addr_pre_dest(addr_pre), pre_hist(hist), access_callee(acc),
          addr_hist_caller(caller), base_path(std::move(bp)) {}
  };

private:
  std::unique_ptr<AbductiveDomain> astate_;
  Substitution subst_;
  std::map<AbstractValue, std::pair<AbstractValue, LazyHeapPath>> rev_subst_;
  std::map<AbstractValue, ValueHistory> hist_map_;
  std::set<AbstractValue> visited_;
  std::vector<CalleeIndexToVisit> array_indices_to_visit_;
  std::optional<AbstractValue> first_error_;
  std::map<AbstractValue, std::set<AbstractValue>> aliases_;
  std::map<AbstractValue, std::vector<LazyHeapPath>> alias_paths_by_caller_;
  bool has_unsupported_alias_{false};

public:
  explicit CallState(std::unique_ptr<AbductiveDomain> astate)
      : astate_(std::move(astate)) {}

  AbductiveDomain &getAstate() { return *astate_; }
  const AbductiveDomain &getAstate() const { return *astate_; }
  std::unique_ptr<AbductiveDomain> takeAstate() { return std::move(astate_); }

  Substitution &getSubst() { return subst_; }
  const Substitution &getSubst() const { return subst_; }

  const std::map<AbstractValue, std::pair<AbstractValue, LazyHeapPath>> &
  getRevSubst() const {
    return rev_subst_;
  }

  const std::map<AbstractValue, ValueHistory> &getHistMap() const {
    return hist_map_;
  }
  std::map<AbstractValue, ValueHistory> &getHistMap() { return hist_map_; }

  const std::set<AbstractValue> &getVisited() const { return visited_; }
  std::set<AbstractValue> &getVisited() { return visited_; }

  const std::vector<CalleeIndexToVisit> &getArrayIndicesToVisit() const {
    return array_indices_to_visit_;
  }
  std::vector<CalleeIndexToVisit> &getArrayIndicesToVisit() {
    return array_indices_to_visit_;
  }

  std::optional<AbstractValue> getFirstError() const { return first_error_; }
  void setFirstError(AbstractValue addr) {
    if (!first_error_) {
      first_error_ = addr;
    }
  }

  const std::map<AbstractValue, std::set<AbstractValue>> &getAliases() const {
    return aliases_;
  }
  std::map<AbstractValue, std::set<AbstractValue>> &getAliases() {
    return aliases_;
  }

  bool hasAliases() const {
    return !aliases_.empty() || !alias_paths_by_caller_.empty();
  }
  bool hasUnsupportedAlias() const { return has_unsupported_alias_; }

  void addAlias(AbstractValue addr_callee, AbstractValue addr_callee_prime) {
    aliases_[addr_callee].insert(addr_callee_prime);
    aliases_[addr_callee_prime].insert(addr_callee);
  }

  void recordAliasConflict(AbstractValue caller_addr,
                           AbstractValue existing_callee,
                           const LazyHeapPath &existing_path,
                           AbstractValue new_callee,
                           const LazyHeapPath &new_path) {
    addAlias(existing_callee, new_callee);
    auto &paths = alias_paths_by_caller_[caller_addr];
    if (paths.empty()) {
      paths.push_back(existing_path);
    }
    paths.push_back(new_path);
    if (!existing_path.isSupported() || !new_path.isSupported()) {
      has_unsupported_alias_ = true;
    }
  }

  std::vector<std::vector<HeapPath>> extractAliasClasses() const {
    std::vector<std::vector<HeapPath>> classes;
    for (const auto &kv : alias_paths_by_caller_) {
      std::set<HeapPath> unique_paths;
      for (const auto &lp : kv.second) {
        if (auto hp = lp.force()) {
          unique_paths.insert(*hp);
        }
      }
      if (unique_paths.size() >= 2) {
        classes.emplace_back(unique_paths.begin(), unique_paths.end());
      }
    }
    return classes;
  }

  void addRevSubst(AbstractValue addr_caller, AbstractValue addr_callee,
                   const LazyHeapPath &path) {
    rev_subst_.emplace(addr_caller, std::make_pair(addr_callee, path));
  }

  std::optional<std::pair<AbstractValue, LazyHeapPath>>
  toCalleeAddr(AbstractValue addr_caller) const {
    AbstractValue canon =
        astate_ ? astate_->getCanonical(addr_caller) : addr_caller;
    auto it = rev_subst_.find(canon);
    if (it != rev_subst_.end()) {
      return it->second;
    }
    it = rev_subst_.find(addr_caller);
    if (it != rev_subst_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  std::optional<std::pair<AbstractValue, ValueHistory>>
  toCallerValue(AbstractValue addr_callee) const {
    auto caller_opt = subst_.substitute(addr_callee);
    if (!caller_opt) {
      return std::nullopt;
    }
    AbstractValue caller_canon =
        astate_ ? astate_->getCanonical(*caller_opt) : *caller_opt;
    ValueHistory hist;
    auto hist_it = hist_map_.find(addr_callee);
    if (hist_it != hist_map_.end()) {
      hist = hist_it->second;
    }
    return std::make_pair(caller_canon, hist);
  }

  IncorporateResult incorporateNewEqs(const NewEqs &new_eqs);
  bool incorporateNewEqs(const PulseFormula &new_eqs);

private:
  void canonicalizeMaps();
};

} // namespace pulse
