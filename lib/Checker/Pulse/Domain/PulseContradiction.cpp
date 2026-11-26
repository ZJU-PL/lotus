#include "Checker/Pulse/Domain/PulseContradiction.h"

#include "Checker/Pulse/Core/PulseCallState.h"
#include "Checker/Pulse/Core/PulseFormula.h"

#include <algorithm>
#include <optional>

namespace pulse {

//===----------------------------------------------------------------------===//
// Contradictions
//
// A contradiction is a reason a particular summary/precondition cannot apply at
// a call site. In a Pulse/incorrectness setting, contradictions must be
// *provable*: rejecting an entry based on heuristics can silently drop feasible
// witnesses (false negatives).
//===----------------------------------------------------------------------===//

std::optional<Contradiction> checkAliasingContradiction(
    const std::map<AbstractValue, AbstractValue> &formal_to_actual_map,
    const PulseFormula &callee_pre_formula) {

  // Build reverse map: actual -> set of formals that map to it
  std::map<AbstractValue, std::set<AbstractValue>> actual_to_formals;
  for (const auto &kv : formal_to_actual_map) {
    AbstractValue formal = kv.first;
    AbstractValue actual = kv.second;
    actual_to_formals[actual].insert(formal);
  }

  // Check if any actual maps to multiple distinct formals
  for (const auto &kv : actual_to_formals) {
    const std::set<AbstractValue> &formals = kv.second;
    if (formals.size() < 2) {
      continue; // Only one formal maps to this actual
    }

    // Check if these formals are known to be distinct in the callee's
    // pre-condition
    std::vector<AbstractValue> formal_vec(formals.begin(), formals.end());
    for (size_t i = 0; i < formal_vec.size(); ++i) {
      for (size_t j = i + 1; j < formal_vec.size(); ++j) {
        AbstractValue formal1 = formal_vec[i];
        AbstractValue formal2 = formal_vec[j];

        // Check if they're known to be equal in pre (if so, no contradiction)
        if (callee_pre_formula.areEqual(formal1, formal2)) {
          continue;
        }

        // Check if they're known to be disequal in pre (contradiction!)
        if (callee_pre_formula.areDisequal(formal1, formal2)) {
          return Contradiction::makeAliasing(kv.first, formal1, formal2);
        }

        // If the precondition does not constrain the aliasing relation,
        // do not reject the entry. Sound incorrectness requires that
        // contradictions are established, not guessed.
      }
    }
  }

  return std::nullopt;
}

std::optional<Contradiction>
checkPathConditionContradiction(const PulseFormula &caller_formula,
                                const PulseFormula &callee_pre_formula) {
  PulseFormula merged = PulseFormula::merge(caller_formula, callee_pre_formula);
  if (!merged.isConsistent() || merged.isUnsat()) {
    return Contradiction::makePathCondition(
        "Merged caller/precondition formula is unsatisfiable");
  }

  return std::nullopt;
}

std::optional<Contradiction> checkContradiction(
    const PulseFormula &caller_formula, const PulseFormula &callee_pre_formula,
    const std::map<AbstractValue, AbstractValue> &formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>
        &actual_to_formals_map) {

  // Check aliasing contradiction (matching Infer's Aliasing and
  // AliasingWithAllAliases)
  auto aliasing_contradiction =
      checkAliasingContradiction(formal_to_actual_map, callee_pre_formula);
  if (aliasing_contradiction) {
    return aliasing_contradiction;
  }

  // Check path condition contradiction (matching Infer's PathCondition)
  auto path_contradiction =
      checkPathConditionContradiction(caller_formula, callee_pre_formula);
  if (path_contradiction) {
    return path_contradiction;
  }

  return std::nullopt;
}

// Old version kept for backward compatibility - delegates to new version
std::optional<Contradiction> checkAliasingWithAllAliases(
    const std::map<AbstractValue, AbstractValue> &formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>
        &actual_to_formals_map,
    const PulseFormula &callee_pre_formula) {
  return checkAliasingWithAllAliases(
      formal_to_actual_map, actual_to_formals_map, callee_pre_formula, nullptr);
}

/**
 * Check for DynamicTypeNeeded contradiction
 * Returns map of heap paths to abstract values that need dynamic type
 * specialization
 */
std::optional<Contradiction> checkDynamicTypeNeeded(
    const std::map<HeapPath, AbstractValue> &heap_paths_to_values) {
  if (!heap_paths_to_values.empty()) {
    return Contradiction::makeDynamicTypeNeeded(heap_paths_to_values);
  }
  return std::nullopt;
}

/**
 * Check for CapturedFormalActualLength contradiction
 */
std::optional<Contradiction> checkCapturedFormalActualLength(
    unsigned captured_formal_count, unsigned captured_actual_count,
    const std::vector<AbstractValue> &captured_formals,
    const std::vector<AbstractValue> &captured_actuals) {

  if (captured_formal_count != captured_actual_count) {
    return Contradiction::makeCapturedFormalActualLength(
        captured_formal_count, captured_actual_count, captured_formals,
        captured_actuals);
  }

  return std::nullopt;
}

/**
 * Merge contradictions: when applying a summary with multiple disjuncts,
 * merge all possible contradictions into a single one.
 */
std::optional<Contradiction>
mergeContradictions(const std::optional<Contradiction> &c1,
                    const std::optional<Contradiction> &c2) {

  if (!c1)
    return c2;
  if (!c2)
    return c1;

  const Contradiction &cont1 = *c1;
  const Contradiction &cont2 = *c2;

  // If both are the same kind, merge them
  if (cont1.kind == cont2.kind) {
    switch (cont1.kind) {
    case ContradictionKind::AliasingWithAllAliases: {
      // Merge alias classes
      std::vector<std::vector<HeapPath>> merged_classes =
          cont1.alias_classes_with_paths;
      merged_classes.insert(merged_classes.end(),
                            cont2.alias_classes_with_paths.begin(),
                            cont2.alias_classes_with_paths.end());
      return Contradiction::makeAliasingWithAllAliases(merged_classes);
    }
    case ContradictionKind::DynamicTypeNeeded: {
      // Merge dynamic type paths and resolved dynamic type names
      std::map<HeapPath, AbstractValue> merged_paths = cont1.dynamic_type_paths;
      merged_paths.insert(cont2.dynamic_type_paths.begin(),
                          cont2.dynamic_type_paths.end());
      Contradiction merged = Contradiction::makeDynamicTypeNeeded(merged_paths);
      merged.dynamic_type_names = cont1.dynamic_type_names;
      merged.dynamic_type_names.insert(cont2.dynamic_type_names.begin(),
                                       cont2.dynamic_type_names.end());
      return merged;
    }
    case ContradictionKind::PathCondition: {
      // Merge conflicting constraints
      std::vector<std::string> merged_conflicts =
          cont1.path_condition.conflicting_constraints;
      merged_conflicts.insert(
          merged_conflicts.end(),
          cont2.path_condition.conflicting_constraints.begin(),
          cont2.path_condition.conflicting_constraints.end());
      std::string merged_reason = cont1.path_condition.unsat_reason + "; " +
                                  cont2.path_condition.unsat_reason;
      return Contradiction::makePathCondition(merged_reason, merged_conflicts);
    }
    default:
      // For other types, return the first one
      return c1;
    }
  }

  // Different kinds: prioritize DynamicTypeNeeded > AliasingWithAllAliases >
  // others
  if (cont1.kind == ContradictionKind::DynamicTypeNeeded)
    return c1;
  if (cont2.kind == ContradictionKind::DynamicTypeNeeded)
    return c2;
  if (cont1.kind == ContradictionKind::AliasingWithAllAliases)
    return c1;
  if (cont2.kind == ContradictionKind::AliasingWithAllAliases)
    return c2;

  // Default: return first
  return c1;
}

/**
 * Check if contradiction is DynamicTypeNeeded and extract the map
 */
std::optional<std::map<HeapPath, AbstractValue>>
isDynamicTypeNeededContradiction(const Contradiction &c) {
  if (c.kind == ContradictionKind::DynamicTypeNeeded) {
    return c.dynamic_type_paths;
  }
  return std::nullopt;
}

/**
 * Enhanced contradiction checking with call_state support
 */
std::optional<Contradiction> checkContradictionWithCallState(
    const PulseFormula &caller_formula, const PulseFormula &callee_pre_formula,
    const std::map<AbstractValue, AbstractValue> &formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>
        &actual_to_formals_map,
    const CallState *call_state) {

  // Use call_state to get heap paths for better aliasing detection
  auto aliasing_contradiction =
      checkAliasingWithAllAliases(formal_to_actual_map, actual_to_formals_map,
                                  callee_pre_formula, call_state);
  if (aliasing_contradiction) {
    return aliasing_contradiction;
  }

  // Fall back to basic contradiction checking
  return checkContradiction(caller_formula, callee_pre_formula,
                            formal_to_actual_map, actual_to_formals_map);
}

/**
 * Check for AliasingWithAllAliases with heap path support
 */
std::optional<Contradiction> checkAliasingWithAllAliases(
    const std::map<AbstractValue, AbstractValue> &formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>
        &actual_to_formals_map,
    const PulseFormula &callee_pre_formula, const CallState *call_state) {

  // Build alias classes with heap paths if call_state is available
  std::vector<std::vector<HeapPath>> alias_classes;
  if (call_state && call_state->hasAliases()) {
    alias_classes = call_state->extractAliasClasses();
    if (!alias_classes.empty()) {
      return Contradiction::makeAliasingWithAllAliases(alias_classes);
    }
  }
  std::set<AbstractValue> processed;

  for (const auto &kv : actual_to_formals_map) {
    const std::set<AbstractValue> &formals = kv.second;
    if (formals.size() < 2) {
      continue;
    }

    // Check if these formals are provably distinct in pre.
    std::vector<AbstractValue> formal_vec(formals.begin(), formals.end());
    bool has_proven_distinct_pair = false;
    bool has_equal_pair = false;

    for (size_t i = 0; i < formal_vec.size(); ++i) {
      for (size_t j = i + 1; j < formal_vec.size(); ++j) {
        AbstractValue formal1 = formal_vec[i];
        AbstractValue formal2 = formal_vec[j];

        if (callee_pre_formula.areEqual(formal1, formal2)) {
          has_equal_pair = true;
          continue;
        }
        if (callee_pre_formula.areDisequal(formal1, formal2)) {
          has_proven_distinct_pair = true;
        }
      }
    }

    if (has_proven_distinct_pair && !has_equal_pair) {
      // Try to get heap paths from call_state when available
      std::vector<HeapPath> paths;
      if (call_state) {
        AbstractValue actual = kv.first;
        auto callee_opt = call_state->toCalleeAddr(actual);
        if (callee_opt) {
          auto path_opt = callee_opt->second.force();
          if (path_opt) {
            paths.push_back(*path_opt);
          }
        }
      }
      // Raise aliasing contradiction only when the callee precondition proves
      // the formals must stay distinct. Heap paths are attached when available.
      alias_classes.push_back(paths);
    }
  }

  if (!alias_classes.empty()) {
    return Contradiction::makeAliasingWithAllAliases(alias_classes);
  }

  return std::nullopt;
}

} // namespace pulse
