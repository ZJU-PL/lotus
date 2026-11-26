#include "Checker/Pulse/Interproc/PulseSpecialization.h"

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Domain/PulseAbductiveDomain.h"
#include "Checker/Pulse/Domain/PulseOperations.h"

#include <algorithm>
#include <map>
#include <set>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

namespace pulse {

namespace {

static std::optional<AbstractValue>
materializeHeapPathInEntry(AbductiveDomain &astate,
                           const llvm::Function *callee, const HeapPath &path,
                           AbstractValueFactory &factory) {
  const auto &elems = path.getPath();
  if (elems.empty() || elems[0].kind != HeapPath::PathElement::Pvar) {
    return std::nullopt;
  }

  AbstractValue cur;
  if (elems[0].param_index >= 0) {
    unsigned idx = static_cast<unsigned>(elems[0].param_index);
    if (!callee || idx >= callee->arg_size()) {
      return std::nullopt;
    }
    const llvm::Argument *arg = callee->getArg(idx);
    if (const Address *addr = astate.getPreStack().find(arg)) {
      cur = astate.getCanonical(addr->addr);
    } else if (const Address *addr = astate.getPostStack().find(arg)) {
      cur = astate.getCanonical(addr->addr);
    } else {
      cur = factory.getOrCreate(arg);
      astate.getPostStack().add(arg, Address(cur));
      astate.getPreStack().add(arg, Address(cur));
    }
    astate.registerPreAddress(cur);
  } else if (elems[0].pvar_root) {
    cur = astate.getCanonical(factory.getOrCreate(elems[0].pvar_root));
    astate.getPostAttrs().add(cur, Attribute::Global);
    astate.registerPreAddress(cur);
  } else {
    return std::nullopt;
  }

  for (size_t i = 1; i < elems.size(); ++i) {
    Access acc;
    switch (elems[i].kind) {
    case HeapPath::PathElement::Dereference:
      acc = Access(AccessKind::Dereference);
      break;
    case HeapPath::PathElement::FieldAccess:
      acc = Access(elems[i].field_idx);
      break;
    case HeapPath::PathElement::ArrayIndex:
      acc = Access::arrayIndex(astate.getCanonical(elems[i].index),
                               elems[i].stride);
      break;
    case HeapPath::PathElement::Pvar:
      return std::nullopt;
    }

    if (const Address *existing = astate.getPostHeap().findEdge(cur, acc)) {
      AbstractValue prev = cur;
      cur = astate.getCanonical(existing->addr);
      astate.abduceToPre(prev, acc, *existing);
    } else {
      AbstractValue fresh = factory.createFresh();
      Address target(fresh);
      astate.getPostHeap().addEdge(cur, acc, target);
      astate.abduceToPre(cur, acc, target);
      astate.abduceAttrToPre(cur, Attribute::Allocated);
      cur = fresh;
    }
  }

  return astate.getCanonical(cur);
}

} // namespace

void SpecializationKey::normalize() {
  // Union-find merge overlapping alias classes and sort deterministically.
  std::vector<std::set<HeapPath>> merged;
  for (const auto &cls : alias_classes) {
    if (cls.size() < 2)
      continue;
    std::set<HeapPath> cur(cls.begin(), cls.end());
    if (cur.size() < 2)
      continue;
    std::vector<size_t> overlapping;
    for (size_t i = 0; i < merged.size(); ++i) {
      for (const auto &hp : cur) {
        if (merged[i].count(hp) > 0) {
          overlapping.push_back(i);
          break;
        }
      }
    }
    for (auto it = overlapping.rbegin(); it != overlapping.rend(); ++it) {
      cur.insert(merged[*it].begin(), merged[*it].end());
      merged.erase(merged.begin() + static_cast<ptrdiff_t>(*it));
    }
    merged.push_back(std::move(cur));
  }

  alias_classes.clear();
  for (const auto &s : merged) {
    alias_classes.emplace_back(s.begin(), s.end());
  }
  std::sort(alias_classes.begin(), alias_classes.end());
}

void SpecializationKey::merge(const SpecializationKey &other) {
  alias_classes.insert(alias_classes.end(), other.alias_classes.begin(),
                       other.alias_classes.end());
  for (const auto &kv : other.dynamic_types_by_path) {
    dynamic_types_by_path.emplace(kv.first, kv.second);
  }
  for (const auto &kv : other.heap_paths_to_values) {
    heap_paths_to_values.emplace(kv.first, kv.second);
  }
  for (const auto &kv : other.aliasing_map) {
    aliasing_map[kv.first].insert(kv.second.begin(), kv.second.end());
  }
  for (const auto &kv : other.dynamic_types) {
    dynamic_types.emplace(kv.first, kv.second);
  }
  normalize();
}

SpecializationKey
SpecializationKey::fromAliases(std::vector<std::vector<HeapPath>> classes) {
  SpecializationKey key;
  key.alias_classes = std::move(classes);
  key.normalize();
  return key;
}

SpecializationKey SpecializationManager::computeSpecializationKey(
    const AbductiveDomain &caller_state, const llvm::CallInst *call,
    const std::vector<AbstractValue> &actual_args) {
  SpecializationKey key;
  if (!call) {
    return key;
  }

  const llvm::Function *callee = call->getCalledFunction();
  auto formalName = [callee](size_t idx) -> std::string {
    if (callee && idx < callee->arg_size() &&
        callee->getArg(static_cast<unsigned>(idx))->hasName()) {
      return callee->getArg(static_cast<unsigned>(idx))->getName().str();
    }
    return "arg" + std::to_string(idx);
  };

  // Group pointer actuals that alias in the caller into callee HeapPath
  // equivalence classes.
  std::map<AbstractValue, std::vector<HeapPath>> actual_to_paths;
  for (size_t i = 0; i < actual_args.size() && i < call->arg_size(); ++i) {
    if (!call->getArgOperand(static_cast<unsigned>(i))
             ->getType()
             ->isPointerTy()) {
      continue;
    }
    AbstractValue canon = caller_state.getCanonical(actual_args[i]);
    HeapPath hp =
        HeapPath::fromFormal(static_cast<unsigned>(i), formalName(i));
    actual_to_paths[canon].push_back(hp);

    if (auto dyn_ty = caller_state.getDynamicType(canon)) {
      key.dynamic_types_by_path[hp] = *dyn_ty;
    }
  }

  for (auto &kv : actual_to_paths) {
    if (kv.second.size() >= 2) {
      key.alias_classes.push_back(std::move(kv.second));
    }
  }

  key.normalize();
  return key;
}

std::optional<AbductiveDomain>
SpecializationManager::applySpecializationToEntry(
    const AbductiveDomain &initial_astate, const llvm::Function *callee,
    const SpecializationKey &key, AbstractValueFactory &factory) {
  AbductiveDomain specialized = initial_astate.clone();

  for (const auto &cls : key.alias_classes) {
    if (cls.size() < 2)
      continue;
    std::vector<AbstractValue> materialized;
    materialized.reserve(cls.size());
    for (const HeapPath &hp : cls) {
      auto av_opt = materializeHeapPathInEntry(specialized, callee, hp, factory);
      if (!av_opt) {
        return std::nullopt;
      }
      materialized.push_back(*av_opt);
    }
    AbstractValue root = materialized[0];
    for (size_t i = 1; i < materialized.size(); ++i) {
      if (!specialized.getPathFormula().addEquality(root, materialized[i])) {
        return std::nullopt;
      }
      auto inc = specialized.incorporatePendingNewEqs();
      if (!inc.isOk()) {
        return std::nullopt;
      }
      root = specialized.getCanonical(root);
    }
  }

  for (const auto &kv : key.dynamic_types_by_path) {
    auto av_opt =
        materializeHeapPathInEntry(specialized, callee, kv.first, factory);
    if (!av_opt) {
      continue;
    }
    specialized.setDynamicType(*av_opt, kv.second);
  }

  if (!specialized.getPathFormula().isConsistent() ||
      specialized.getPathFormula().isUnsat()) {
    return std::nullopt;
  }

  specialized.canonicalize();
  return specialized;
}

AbductiveDomain
SpecializationManager::applySpecialization(const AbductiveDomain &astate,
                                           const SpecializationKey &key) {
  AbductiveDomain specialized = astate.clone();

  for (const auto &kv : key.aliasing_map) {
    AbstractValue v1 = kv.first;
    for (AbstractValue v2 : kv.second) {
      (void)specialized.getPathFormula().addEquality(v1, v2);
    }
  }
  (void)specialized.incorporatePendingNewEqs();

  for (const auto &kv : key.dynamic_types) {
    specialized.setDynamicType(kv.first, kv.second);
    specialized.addNeedDynamicTypeSpecialization(kv.first);
  }

  for (const auto &kv : key.heap_paths_to_values) {
    specialized.addNeedDynamicTypeSpecialization(
        specialized.getCanonical(kv.second));
  }

  specialized.canonicalize();
  return specialized;
}

} // namespace pulse
