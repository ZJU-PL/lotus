/*
 * LotusAA - Function Pointer Results Implementation
 */

#include "Alias/LotusAA/Support/FunctionPointerResults.h"

using namespace llvm;

CallTargetSet *FunctionPointerResults::getTargets(Function *caller, Value *callsite) {
  auto callerIt = results_.find(caller);
  if (callerIt == results_.end())
    return nullptr;

  auto callsiteIt = callerIt->second.find(callsite);
  if (callsiteIt == callerIt->second.end())
    return nullptr;

  return &callsiteIt->second;
}

const CallTargetSet *FunctionPointerResults::getTargets(Function *caller, Value *callsite) const {
  auto callerIt = results_.find(caller);
  if (callerIt == results_.end())
    return nullptr;

  auto callsiteIt = callerIt->second.find(callsite);
  if (callsiteIt == callerIt->second.end())
    return nullptr;

  return &callsiteIt->second;
}

void FunctionPointerResults::addTarget(Function *caller, Value *callsite, Function *target) {
  if (caller && callsite && target) {
    results_[caller][callsite].insert(target);
  }
}

void FunctionPointerResults::setTargets(Function *caller, Value *callsite, 
                                        const CallTargetSet &targets) {
  if (caller && callsite) {
    results_[caller][callsite] = targets;
  }
}

CallSiteTargetMap *FunctionPointerResults::getCallSites(Function *caller) {
  auto it = results_.find(caller);
  return (it != results_.end()) ? &it->second : nullptr;
}

const CallSiteTargetMap *FunctionPointerResults::getCallSites(Function *caller) const {
  auto it = results_.find(caller);
  return (it != results_.end()) ? &it->second : nullptr;
}

bool FunctionPointerResults::hasChanged(Function *caller,
                                        const CallSiteTargetMap &newResults,
                                        std::set<Function *> &outChangedCallers) const {
  auto callerIt = results_.find(caller);
  
  // No previous results means everything is new
  if (callerIt == results_.end()) {
    if (!newResults.empty()) {
      outChangedCallers.insert(caller);
      return true;
    }
    return false;
  }

  const CallSiteTargetMap &oldResults = callerIt->second;
  
  // Check each call site in new results
  for (const auto &newCallsite : newResults) {
    Value *callsite = newCallsite.first;
    const CallTargetSet &newTargets = newCallsite.second;
    
    auto oldCallsiteIt = oldResults.find(callsite);
    if (oldCallsiteIt == oldResults.end()) {
      // New call site
      if (!newTargets.empty()) {
        outChangedCallers.insert(caller);
        return true;
      }
      continue;
    }
    
    const CallTargetSet &oldTargets = oldCallsiteIt->second;
    
    // Check if target sets differ
    if (oldTargets.size() != newTargets.size()) {
      outChangedCallers.insert(caller);
      return true;
    }
    
    for (Function *newTarget : newTargets) {
      if (oldTargets.count(newTarget) == 0) {
        outChangedCallers.insert(caller);
        return true;
      }
    }
  }
  
  // Check for removed call sites
  for (const auto &oldCallsite : oldResults) {
    if (newResults.find(oldCallsite.first) == newResults.end()) {
      if (!oldCallsite.second.empty()) {
        outChangedCallers.insert(caller);
        return true;
      }
    }
  }
  
  return false;
}

bool FunctionPointerResults::updateAndDetectChanges(Function *caller,
                                                    const CallSiteTargetMap &newResults) {
  std::set<Function *> changedCallers;
  bool changed = hasChanged(caller, newResults, changedCallers);
  
  if (changed) {
    results_[caller] = newResults;
  }
  
  return changed;
}

void FunctionPointerResults::clear() {
  results_.clear();
}

size_t FunctionPointerResults::getCallSiteCount() const {
  size_t count = 0;
  for (const auto &callerPair : results_) {
    count += callerPair.second.size();
  }
  return count;
}

