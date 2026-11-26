#include "Checker/Pulse/Interproc/PulseTransitiveInfo.h"

#include <llvm/Support/JSON.h>

namespace pulse {

llvm::Expected<std::vector<TransitiveAccessPolicy>>
loadTransitiveAccessPoliciesFromJson(llvm::StringRef json_text) {
  auto parsed = llvm::json::parse(json_text);
  if (!parsed) {
    return parsed.takeError();
  }
  const auto *root = parsed->getAsObject();
  if (!root) {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "transitive-access config JSON must be an object");
  }
  std::vector<TransitiveAccessPolicy> policies;
  const auto *arr = root->getArray("policies");
  if (!arr) {
    return policies;
  }
  for (const auto &v : *arr) {
    const auto *obj = v.getAsObject();
    if (!obj)
      continue;
    TransitiveAccessPolicy pol;
    pol.name = obj->getString("name").getValueOr("TransitivePolicy").str();
    pol.description =
        obj->getString("description")
            .getValueOr("Forbidden transitive call detected")
            .str();
    if (const auto *srcs = obj->getArray("source_functions")) {
      for (const auto &sv : *srcs) {
        if (auto s = sv.getAsString())
          pol.source_functions.push_back(s->str());
      }
    }
    if (auto sr = obj->getString("source_regex")) {
      pol.source_functions.push_back(sr->str());
    }
    if (const auto *forb = obj->getArray("forbidden_callees")) {
      for (const auto &fv : *forb) {
        if (auto s = fv.getAsString())
          pol.forbidden_callees.push_back(s->str());
      }
    }
    if (auto fr = obj->getString("forbidden_callee_regex")) {
      pol.forbidden_callees.push_back(fr->str());
    }
    if (!pol.source_functions.empty() && !pol.forbidden_callees.empty()) {
      policies.push_back(std::move(pol));
    }
  }
  return policies;
}

void TransitiveInfo::recordTransitiveAccess(const llvm::Instruction *loc,
                                            const llvm::Function *func) {
  for (const auto &a : transitive_accesses_) {
    if (a.location == loc && a.function == func) {
      return;
    }
  }
  if (transitive_accesses_.size() >= kMaxTransitiveEntries) {
    return;
  }
  transitive_accesses_.emplace_back(loc, func);
}

void TransitiveInfo::recordCallResolution(
    const llvm::Instruction *call_site, const llvm::Function *caller,
    CallResolution resolution,
    const std::vector<const llvm::Function *> &targets) {
  for (auto &existing : call_resolutions_) {
    if (existing.call_site == call_site && existing.caller == caller &&
        existing.resolution == resolution) {
      for (const llvm::Function *t : targets) {
        bool found = false;
        for (const llvm::Function *et : existing.targets) {
          if (et == t) {
            found = true;
            break;
          }
        }
        if (!found && existing.targets.size() < 16) {
          existing.targets.push_back(t);
        }
      }
      return;
    }
  }
  if (call_resolutions_.size() >= kMaxTransitiveEntries) {
    return;
  }
  CallInfo info(call_site, caller, resolution);
  for (const llvm::Function *t : targets) {
    bool found = false;
    for (const llvm::Function *et : info.targets) {
      if (et == t) {
        found = true;
        break;
      }
    }
    if (!found && info.targets.size() < 16) {
      info.targets.push_back(t);
    }
  }
  call_resolutions_.push_back(std::move(info));
}

TransitiveInfo TransitiveInfo::merge(const TransitiveInfo &t1,
                                     const TransitiveInfo &t2) {
  if (t2.transitive_accesses_.empty() && t2.call_resolutions_.empty()) {
    return t1.clone();
  }
  if (t1.transitive_accesses_.empty() && t1.call_resolutions_.empty()) {
    return t2.clone();
  }

  TransitiveInfo merged = t1.clone();
  for (const auto &a : t2.transitive_accesses_) {
    merged.recordTransitiveAccess(a.location, a.function);
  }
  for (const auto &c : t2.call_resolutions_) {
    merged.recordCallResolution(c.call_site, c.caller, c.resolution, c.targets);
  }
  return merged;
}

} // namespace pulse
