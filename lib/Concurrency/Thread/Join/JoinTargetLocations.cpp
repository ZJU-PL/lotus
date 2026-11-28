/**
 * @file JoinTargetLocations.cpp
 * @brief Handle tracing and location resolution for join-target analysis
 */

#include "Concurrency/Thread/Join/JoinTargetAnalysis.h"

#include <deque>
#include <set>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace mhp {

namespace {

void collectDirectCallActuals(const Function *parent, unsigned argNo,
                              std::deque<const Value *> &worklist,
                              bool &expanded) {
  if (!parent) {
    return;
  }
  SmallVector<const Value *, 8> funcRefs;
  SmallPtrSet<const Value *, 16> seenRefs;
  funcRefs.push_back(parent);
  seenRefs.insert(parent);
  while (!funcRefs.empty()) {
    const Value *ref = funcRefs.pop_back_val();
    for (const Use &use : ref->uses()) {
      const User *user = use.getUser();
      if (const auto *cb = dyn_cast<CallBase>(user)) {
        if (cb->getCalledOperand() == ref && argNo < cb->arg_size()) {
          worklist.push_back(cb->getArgOperand(argNo));
          expanded = true;
        }
      } else if (const auto *ce = dyn_cast<ConstantExpr>(user)) {
        if (ce->isCast() && seenRefs.insert(ce).second) {
          funcRefs.push_back(ce);
        }
      }
    }
  }
}

bool collectUniqueStoredValues(const Value *ptr,
                               SmallVectorImpl<const Value *> &storedValues) {
  if (!ptr) {
    return false;
  }

  std::set<const Value *> uniqueValues;
  for (const User *user : ptr->users()) {
    const auto *store = dyn_cast<StoreInst>(user);
    if (!store || store->getPointerOperand() != ptr) {
      continue;
    }
    uniqueValues.insert(store->getValueOperand());
    if (uniqueValues.size() > 1) {
      storedValues.clear();
      return false;
    }
  }

  for (const Value *value : uniqueValues) {
    storedValues.push_back(value);
  }
  return !storedValues.empty();
}

} // namespace

std::size_t HandleLocationHash::operator()(
    const HandleLocation &location) const {
  std::size_t seed = std::hash<const Value *>{}(location.base);
  seed ^= std::hash<bool>{}(location.is_base_wildcard) + 0x9e3779b9 +
          (seed << 6) + (seed >> 2);
  for (int64_t offset : location.offsets) {
    seed ^= std::hash<int64_t>{}(offset) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
  }
  return seed;
}

std::size_t SummaryLocationHash::operator()(
    const SummaryLocation &location) const {
  std::size_t seed = std::hash<unsigned>{}(location.arg_no);
  seed ^= std::hash<bool>{}(location.is_base_wildcard) + 0x9e3779b9 +
          (seed << 6) + (seed >> 2);
  for (int64_t offset : location.offsets) {
    seed ^= std::hash<int64_t>{}(offset) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
  }
  return seed;
}

const Value *JoinTargetAnalysis::traceThreadHandleRoot(const Value *value,
                                                       const Module *module) {
  if (!value) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  const Value *resolvedRoot = nullptr;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    const Value *stripped = current->stripPointerCasts();
    if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped)) {
      if (!resolvedRoot) {
        resolvedRoot = stripped;
      } else if (resolvedRoot != stripped) {
        return nullptr;
      }
      continue;
    }

    if (const auto *load = dyn_cast<LoadInst>(stripped)) {
      SmallVector<const Value *, 2> storedValues;
      if (collectUniqueStoredValues(load->getPointerOperand(), storedValues)) {
        for (const Value *stored : storedValues) {
          worklist.push_back(stored);
        }
      } else {
        worklist.push_back(load->getPointerOperand());
      }
      continue;
    }

    if (const auto *store = dyn_cast<StoreInst>(stripped)) {
      worklist.push_back(store->getPointerOperand());
      continue;
    }

    if (const auto *gep = dyn_cast<GetElementPtrInst>(stripped)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    if (const auto *phi = dyn_cast<PHINode>(stripped)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }

    if (const auto *select = dyn_cast<SelectInst>(stripped)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }

    if (const auto *arg = dyn_cast<Argument>(stripped)) {
      if (!module) {
        return stripped;
      }
      const Function *parent = arg->getParent();
      bool expanded = false;
      collectDirectCallActuals(parent, arg->getArgNo(), worklist, expanded);
      if (!expanded) {
        if (!resolvedRoot) {
          resolvedRoot = stripped;
        } else if (resolvedRoot != stripped) {
          return nullptr;
        }
      }
      continue;
    }

    if (const auto *inst = dyn_cast<Instruction>(stripped)) {
      for (const Use &operand : inst->operands()) {
        worklist.push_back(operand.get());
      }
      continue;
    }

    if (!resolvedRoot) {
      resolvedRoot = stripped;
    } else if (resolvedRoot != stripped) {
      return nullptr;
    }
  }

  return resolvedRoot;
}

void JoinTargetAnalysis::traceThreadHandleRoots(
    const Value *value, const Module *module,
    std::unordered_set<const Value *> &roots) {
  if (!value) {
    return;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    const Value *stripped = current->stripPointerCasts();
    if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped)) {
      roots.insert(stripped);
      continue;
    }

    if (const auto *load = dyn_cast<LoadInst>(stripped)) {
      SmallVector<const Value *, 2> storedValues;
      if (collectUniqueStoredValues(load->getPointerOperand(), storedValues)) {
        for (const Value *stored : storedValues) {
          worklist.push_back(stored);
        }
      } else {
        worklist.push_back(load->getPointerOperand());
      }
      continue;
    }

    if (const auto *store = dyn_cast<StoreInst>(stripped)) {
      worklist.push_back(store->getPointerOperand());
      worklist.push_back(store->getValueOperand());
      continue;
    }

    if (const auto *gep = dyn_cast<GetElementPtrInst>(stripped)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    if (const auto *phi = dyn_cast<PHINode>(stripped)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }

    if (const auto *select = dyn_cast<SelectInst>(stripped)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }

    if (const auto *arg = dyn_cast<Argument>(stripped)) {
      const Function *parent = arg->getParent();
      if (module && parent) {
        bool expanded = false;
        collectDirectCallActuals(parent, arg->getArgNo(), worklist, expanded);
      }
      continue;
    }

    if (const auto *inst = dyn_cast<Instruction>(stripped)) {
      for (const Use &op : inst->operands()) {
        worklist.push_back(op.get());
      }
      continue;
    }
  }
}

namespace {

using LocationSet = std::unordered_set<HandleLocation, HandleLocationHash>;

LocationSet resolveWriteLocationsImpl(
    const Value *value, const DataLayout &layout,
    SmallPtrSetImpl<const Value *> &visiting,
    std::unordered_map<const Value *, LocationSet> &memo) {
  LocationSet locations;
  if (!value) {
    return locations;
  }

  const Value *stripped = value->stripPointerCasts();
  auto memoIt = memo.find(stripped);
  if (memoIt != memo.end()) {
    return memoIt->second;
  }
  if (!visiting.insert(stripped).second) {
    return locations;
  }

  if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped) ||
      isa<Argument>(stripped)) {
    locations.insert(HandleLocation{stripped, {}, false});
    visiting.erase(stripped);
    memo.emplace(stripped, locations);
    return locations;
  }

  if (const auto *gep = dyn_cast<GEPOperator>(stripped)) {
    auto baseLocations =
        resolveWriteLocationsImpl(gep->getPointerOperand(), layout, visiting, memo);
    if (baseLocations.empty()) {
      if (const Value *baseRoot =
              JoinTargetAnalysis::traceThreadHandleRoot(gep->getPointerOperand(),
                                                        nullptr)) {
        locations.insert(HandleLocation{baseRoot, {}, true});
      }
      visiting.erase(stripped);
      return locations;
    }

    APInt byteOffset(layout.getIndexTypeSizeInBits(gep->getType()), 0, true);
    const bool hasConstantOffset =
        gep->accumulateConstantOffset(layout, byteOffset) &&
        byteOffset.isSignedIntN(64);

    for (const HandleLocation &baseLocation : baseLocations) {
      HandleLocation location = baseLocation;
      if (!hasConstantOffset || location.is_base_wildcard) {
        location.offsets.clear();
        location.is_base_wildcard = true;
      } else {
        int64_t totalOffset = byteOffset.getSExtValue();
        if (!location.offsets.empty()) {
          totalOffset += location.offsets.front();
        }
        location.offsets.clear();
        if (totalOffset != 0) {
          location.offsets.push_back(totalOffset);
        }
      }
      locations.insert(std::move(location));
    }
    visiting.erase(stripped);
    memo.emplace(stripped, locations);
    return locations;
  }

  if (const auto *phi = dyn_cast<PHINode>(stripped)) {
    bool hitCycle = false;
    for (const Value *incoming : phi->incoming_values()) {
      const Value *incStripped = incoming ? incoming->stripPointerCasts() : nullptr;
      if (incStripped && visiting.count(incStripped)) {
        hitCycle = true;
        continue;
      }
      auto incomingLocations =
          resolveWriteLocationsImpl(incoming, layout, visiting, memo);
      locations.insert(incomingLocations.begin(), incomingLocations.end());
    }
    if (hitCycle) {
      LocationSet widened;
      for (HandleLocation loc : locations) {
        loc.offsets.clear();
        loc.is_base_wildcard = true;
        widened.insert(std::move(loc));
      }
      locations = std::move(widened);
    }
    visiting.erase(stripped);
    if (!hitCycle) {
      memo.emplace(stripped, locations);
    }
    return locations;
  }

  if (const auto *select = dyn_cast<SelectInst>(stripped)) {
    auto trueLocations =
        resolveWriteLocationsImpl(select->getTrueValue(), layout, visiting, memo);
    auto falseLocations =
        resolveWriteLocationsImpl(select->getFalseValue(), layout, visiting, memo);
    locations.insert(trueLocations.begin(), trueLocations.end());
    locations.insert(falseLocations.begin(), falseLocations.end());
    visiting.erase(stripped);
    memo.emplace(stripped, locations);
    return locations;
  }

  std::unordered_set<const Value *> roots;
  JoinTargetAnalysis::traceThreadHandleRoots(value, nullptr, roots);
  if (roots.empty()) {
    if (const Value *root =
            JoinTargetAnalysis::traceThreadHandleRoot(value, nullptr)) {
      locations.insert(HandleLocation{root, {}, true});
    }
  } else {
    for (const Value *root : roots) {
      locations.insert(HandleLocation{root, {}, true});
    }
  }
  visiting.erase(stripped);
  memo.emplace(stripped, locations);
  return locations;
}

LocationSet resolveReadLocationsImpl(
    const Value *value, const DataLayout &layout,
    SmallPtrSetImpl<const Value *> &visiting,
    std::unordered_map<const Value *, LocationSet> &readMemo,
    std::unordered_map<const Value *, LocationSet> &writeMemo) {
  LocationSet locations;
  if (!value) {
    return locations;
  }

  const Value *stripped = value->stripPointerCasts();
  auto memoIt = readMemo.find(stripped);
  if (memoIt != readMemo.end()) {
    return memoIt->second;
  }
  if (!visiting.insert(stripped).second) {
    return locations;
  }

  if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped) ||
      isa<Argument>(stripped)) {
    locations.insert(HandleLocation{stripped, {}, false});
    visiting.erase(stripped);
    readMemo.emplace(stripped, locations);
    return locations;
  }

  if (const auto *load = dyn_cast<LoadInst>(stripped)) {
    SmallPtrSet<const Value *, 16> writeVisiting;
    locations = resolveWriteLocationsImpl(load->getPointerOperand(), layout,
                                          writeVisiting, writeMemo);
    visiting.erase(stripped);
    readMemo.emplace(stripped, locations);
    return locations;
  }

  if (const auto *gep = dyn_cast<GEPOperator>(stripped)) {
    SmallPtrSet<const Value *, 16> writeVisiting;
    locations = resolveWriteLocationsImpl(gep, layout, writeVisiting, writeMemo);
    visiting.erase(stripped);
    readMemo.emplace(stripped, locations);
    return locations;
  }

  if (const auto *phi = dyn_cast<PHINode>(stripped)) {
    for (const Value *incoming : phi->incoming_values()) {
      auto incomingLocations = resolveReadLocationsImpl(
          incoming, layout, visiting, readMemo, writeMemo);
      locations.insert(incomingLocations.begin(), incomingLocations.end());
    }
    visiting.erase(stripped);
    readMemo.emplace(stripped, locations);
    return locations;
  }

  if (const auto *select = dyn_cast<SelectInst>(stripped)) {
    auto trueLocations = resolveReadLocationsImpl(
        select->getTrueValue(), layout, visiting, readMemo, writeMemo);
    auto falseLocations = resolveReadLocationsImpl(
        select->getFalseValue(), layout, visiting, readMemo, writeMemo);
    locations.insert(trueLocations.begin(), trueLocations.end());
    locations.insert(falseLocations.begin(), falseLocations.end());
    visiting.erase(stripped);
    readMemo.emplace(stripped, locations);
    return locations;
  }

  visiting.erase(stripped);
  readMemo.emplace(stripped, locations);
  return locations;
}

} // namespace

std::unordered_set<HandleLocation, HandleLocationHash>
JoinTargetAnalysis::resolveReadLocations(const Value *value) const {
  SmallPtrSet<const Value *, 16> visiting;
  std::unordered_map<const Value *, LocationSet> readMemo;
  std::unordered_map<const Value *, LocationSet> writeMemo;
  return resolveReadLocationsImpl(value, m_module.getDataLayout(), visiting,
                                  readMemo, writeMemo);
}

std::unordered_set<HandleLocation, HandleLocationHash>
JoinTargetAnalysis::resolveWriteLocations(const Value *value) const {
  SmallPtrSet<const Value *, 16> visiting;
  std::unordered_map<const Value *, LocationSet> writeMemo;
  return resolveWriteLocationsImpl(value, m_module.getDataLayout(), visiting,
                                   writeMemo);
}

} // namespace mhp
