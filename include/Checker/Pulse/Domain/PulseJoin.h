/** @file PulseJoin.h @brief Join operations for Pulse abstract domains. */
#pragma once

#include "Checker/Pulse/Core/PulsePathContext.h"
#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Domain/PulseOperations.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>

namespace pulse {

/**
 * PulseJoin: Production-ready join operation matching Infer's design.
 *
 * Joins two abstract states, handling:
 * - Value substitution and canonicalization
 * - Heap graph merging
 * - Attribute joining (one-sided vs two-sided)
 * - Path context merging
 * - Formula joining
 */
class PulseJoin {
public:
  /**
   * Join two abductive domains with their path contexts.
   * Returns joined domain and path context, or None if join fails.
   */
  static std::optional<std::pair<AbductiveDomain, PathContext>>
  join(const AbductiveDomain &lhs, const PathContext &path_lhs,
       const AbductiveDomain &rhs, const PathContext &path_rhs);

  /**
   * Join two abductive domains (without path context).
   * Returns joined domain or None if join fails.
   */
  static std::optional<AbductiveDomain>
  joinAbductive(const AbductiveDomain &lhs, const AbductiveDomain &rhs);

  /**
   * Join two summaries.
   */
  static AbductiveDomain joinSummaries(const AbductiveDomain &lhs,
                                       const AbductiveDomain &rhs);

private:
  // Join state: tracks substitutions during join
  struct JoinState {
    static constexpr uint32_t kNullOptId = 0xFFFFFFFDu;
    static uint64_t packOptPair(std::optional<AbstractValue> lhs,
                                std::optional<AbstractValue> rhs) {
      const uint64_t l = lhs ? static_cast<uint64_t>(lhs->getId())
                             : static_cast<uint64_t>(kNullOptId);
      const uint64_t r = rhs ? static_cast<uint64_t>(rhs->getId())
                             : static_cast<uint64_t>(kNullOptId);
      return (l << 32) | r;
    }

    // Maps packed (lhs_value, rhs_value) -> joined_value
    llvm::DenseMap<uint64_t, AbstractValue> subst;

    // Reverse map: joined_value -> (lhs_value, rhs_value)
    llvm::DenseMap<AbstractValue, std::pair<std::optional<AbstractValue>,
                                            std::optional<AbstractValue>>>
        rev_subst;

    // Visited packed pairs to avoid cycles
    llvm::DenseSet<uint64_t> visited;

    AbstractValueFactory *factory;

    JoinState(AbstractValueFactory *f) : factory(f) {}
  };

  /**
   * Join two values with their histories (mutates `state` in place).
   */
  static std::pair<AbstractValue, ValueHistory>
  joinValuesHists(JoinState &state, const AbstractValue &lhs_val,
                  const ValueHistory &lhs_hist, const AbstractValue &rhs_val,
                  const ValueHistory &rhs_hist);

  /**
   * Join optional values (handles one-sided cases; mutates `state` in place).
   */
  static std::pair<AbstractValue, ValueHistory>
  joinValuesHistsOpts(
      JoinState &state, const AbductiveDomain &lhs_astate,
      const std::optional<std::pair<AbstractValue, ValueHistory>> &lhs_opt,
      const AbductiveDomain &rhs_astate,
      const std::optional<std::pair<AbstractValue, ValueHistory>> &rhs_opt);

  /**
   * Join heaps iteratively in place with bounded worklist (prevents stack
   * overflow and product-graph blowup).
   */
  static void joinHeaps(
      JoinState &state, Heap &heap_join, const AbductiveDomain &lhs_astate,
      const std::optional<std::pair<AbstractValue, ValueHistory>> &lhs_opt,
      const AbductiveDomain &rhs_astate,
      const std::optional<std::pair<AbstractValue, ValueHistory>> &rhs_opt,
      bool use_pre_heap = false);

  /**
   * Join pre stacks and pre heaps in place.
   */
  static std::pair<Stack, Heap>
  joinStacks(JoinState &state, const AbductiveDomain &lhs_astate,
             const AbductiveDomain &rhs_astate);

  /**
   * Join attributes: handles one-sided vs two-sided cases.
   */
  static AttributeSet
  joinAttributes(JoinState &state, const AbductiveDomain &lhs_astate,
                 const AbductiveDomain &rhs_astate, bool use_pre_attrs,
                 AbstractValue joined_addr,
                 std::optional<AbstractValue> lhs_addr_opt,
                 std::optional<AbstractValue> rhs_addr_opt);

  /**
   * Join one-sided attribute (only in one branch).
   */
  static std::optional<Attribute> joinOneSidedAttribute(Attribute attr);

  /**
   * Join two-sided attribute (in both branches).
   */
  static std::optional<Attribute>
  joinTwoSidedAttribute(JoinState &state, Attribute attr1, Attribute attr2,
                        AbstractValue lhs_val, AbstractValue rhs_val);

  /**
   * Join formulas.
   */
  static PulseFormula joinFormulas(const AbductiveDomain &lhs,
                                   const AbductiveDomain &rhs);
};

} // namespace pulse

