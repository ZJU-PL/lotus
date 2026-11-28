/** @file PulseFormula.h @brief Symbolic formula representation for Pulse abstract values. */
#pragma once

#include "Checker/Pulse/Core/PulseAbstractValue.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/DenseSet.h>

namespace pulse {

namespace detail {

/// Compact sorted flat map backed by a contiguous std::vector.
/// Zero heap allocation when empty, O(1) append for monotonically growing
/// AbstractValue IDs, single memcpy on copy, single free on teardown, and
/// deterministic sorted iteration order.
template <typename K, typename V> class FlatMap {
public:
  using value_type = std::pair<K, V>;
  using iterator = typename std::vector<value_type>::iterator;
  using const_iterator = typename std::vector<value_type>::const_iterator;
  using reverse_iterator = typename std::vector<value_type>::reverse_iterator;
  using const_reverse_iterator =
      typename std::vector<value_type>::const_reverse_iterator;

private:
  std::vector<value_type> data_;

  iterator lowerBound(const K &k) {
    return std::lower_bound(
        data_.begin(), data_.end(), k,
        [](const value_type &elem, const K &key) { return elem.first < key; });
  }

  const_iterator lowerBound(const K &k) const {
    return std::lower_bound(
        data_.begin(), data_.end(), k,
        [](const value_type &elem, const K &key) { return elem.first < key; });
  }

public:
  bool empty() const { return data_.empty(); }
  size_t size() const { return data_.size(); }
  void clear() { data_.clear(); }
  void reserve(size_t n) { data_.reserve(n); }
  void swap(FlatMap &other) noexcept { data_.swap(other.data_); }

  iterator begin() { return data_.begin(); }
  iterator end() { return data_.end(); }
  const_iterator begin() const { return data_.begin(); }
  const_iterator end() const { return data_.end(); }
  reverse_iterator rbegin() { return data_.rbegin(); }
  reverse_iterator rend() { return data_.rend(); }
  const_reverse_iterator rbegin() const { return data_.rbegin(); }
  const_reverse_iterator rend() const { return data_.rend(); }

  iterator find(const K &k) {
    auto it = lowerBound(k);
    return (it != data_.end() && it->first == k) ? it : data_.end();
  }

  const_iterator find(const K &k) const {
    auto it = lowerBound(k);
    return (it != data_.end() && it->first == k) ? it : data_.end();
  }

  size_t count(const K &k) const { return find(k) != data_.end() ? 1 : 0; }

  const V &at(const K &k) const {
    auto it = find(k);
    assert(it != data_.end());
    return it->second;
  }

  V &operator[](const K &k) {
    if (data_.empty() || data_.back().first < k) {
      data_.emplace_back(k, V{});
      return data_.back().second;
    }
    auto it = lowerBound(k);
    if (it != data_.end() && it->first == k) {
      return it->second;
    }
    return data_.emplace(it, k, V{})->second;
  }

  std::pair<iterator, bool> insert(const value_type &kv) {
    if (data_.empty() || data_.back().first < kv.first) {
      data_.push_back(kv);
      return {std::prev(data_.end()), true};
    }
    auto it = lowerBound(kv.first);
    if (it != data_.end() && it->first == kv.first) {
      return {it, false};
    }
    return {data_.insert(it, kv), true};
  }

  template <typename... Args>
  std::pair<iterator, bool> emplace(const K &k, Args &&...args) {
    if (data_.empty() || data_.back().first < k) {
      data_.emplace_back(std::piecewise_construct, std::forward_as_tuple(k),
                         std::forward_as_tuple(std::forward<Args>(args)...));
      return {std::prev(data_.end()), true};
    }
    auto it = lowerBound(k);
    if (it != data_.end() && it->first == k) {
      return {it, false};
    }
    return {data_.emplace(it, std::piecewise_construct,
                          std::forward_as_tuple(k),
                          std::forward_as_tuple(std::forward<Args>(args)...)),
            true};
  }

  size_t erase(const K &k) {
    auto it = find(k);
    if (it == data_.end())
      return 0;
    data_.erase(it);
    return 1;
  }

  iterator erase(iterator it) { return data_.erase(it); }

  void assignUnsorted(std::vector<value_type> &&items) {
    std::sort(items.begin(), items.end(),
              [](const value_type &a, const value_type &b) {
                return a.first < b.first;
              });
    auto out = items.begin();
    for (auto it = items.begin(); it != items.end(); ++it) {
      if (out != items.begin() && std::prev(out)->first == it->first) {
        std::prev(out)->second = std::move(it->second);
      } else {
        if (out != it)
          *out = std::move(*it);
        ++out;
      }
    }
    items.erase(out, items.end());
    data_.swap(items);
  }

  bool operator==(const FlatMap &other) const { return data_ == other.data_; }
};

/// Compact sorted flat set backed by a contiguous std::vector.
template <typename K> class FlatSet {
public:
  using value_type = K;
  using iterator = typename std::vector<K>::iterator;
  using const_iterator = typename std::vector<K>::const_iterator;

private:
  std::vector<K> data_;

public:
  bool empty() const { return data_.empty(); }
  size_t size() const { return data_.size(); }
  void clear() { data_.clear(); }
  void reserve(size_t n) { data_.reserve(n); }
  void swap(FlatSet &other) noexcept { data_.swap(other.data_); }

  iterator begin() { return data_.begin(); }
  iterator end() { return data_.end(); }
  const_iterator begin() const { return data_.begin(); }
  const_iterator end() const { return data_.end(); }

  const_iterator find(const K &k) const {
    auto it = std::lower_bound(data_.begin(), data_.end(), k);
    return (it != data_.end() && *it == k) ? it : data_.end();
  }

  size_t count(const K &k) const { return find(k) != data_.end() ? 1 : 0; }

  std::pair<iterator, bool> insert(const K &k) {
    if (data_.empty() || data_.back() < k) {
      data_.push_back(k);
      return {std::prev(data_.end()), true};
    }
    auto it = std::lower_bound(data_.begin(), data_.end(), k);
    if (it != data_.end() && *it == k) {
      return {it, false};
    }
    return {data_.insert(it, k), true};
  }

  size_t erase(const K &k) {
    auto it = std::lower_bound(data_.begin(), data_.end(), k);
    if (it == data_.end() || !(*it == k))
      return 0;
    data_.erase(it);
    return 1;
  }

  iterator erase(iterator it) { return data_.erase(it); }

  void assignUnsorted(std::vector<K> &&items) {
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());
    data_.swap(items);
  }

  bool operator==(const FlatSet &other) const { return data_ == other.data_; }
};

} // namespace detail

class Substitution;
class AbstractValueFactory;

/**
 * Linear arithmetic term: represents a linear combination like 2*x + 3*y + 5
 */
struct LinearTerm {
  AbstractValue var;
  int64_t coefficient;

  LinearTerm(AbstractValue v, int64_t c) : var(v), coefficient(c) {}
  bool operator<(const LinearTerm &other) const {
    if (!(var == other.var))
      return var < other.var;
    return coefficient < other.coefficient;
  }
  bool operator==(const LinearTerm &other) const {
    return var == other.var && coefficient == other.coefficient;
  }
};

/**
 * Linear constraint: represents a linear inequality like 2*x + 3*y <= 5
 */
enum class ConstraintKind {
  LessEqual,    // <=
  Less,         // <
  GreaterEqual, // >=
  Greater,      // >
  Equal         // ==
};

struct LinearConstraint {
  std::vector<LinearTerm> terms; // Left-hand side terms
  int64_t constant;              // Right-hand side constant
  ConstraintKind kind;

  LinearConstraint() : constant(0), kind(ConstraintKind::Equal) {}
};

/**
 * Newly discovered equality/zero facts communicated from the formula solver to
 * the memory domain (Infer's Formula.new_eq).
 */
struct NewEq {
  enum class Kind { Equal, EqZero };
  Kind kind{Kind::Equal};
  AbstractValue lhs; // v_old (for Equal) or v (for EqZero)
  AbstractValue rhs; // v_new (for Equal)

  static NewEq makeEqual(AbstractValue old_v, AbstractValue new_v) {
    NewEq eq;
    eq.kind = Kind::Equal;
    eq.lhs = old_v;
    eq.rhs = new_v;
    return eq;
  }

  static NewEq makeEqZero(AbstractValue v) {
    NewEq eq;
    eq.kind = Kind::EqZero;
    eq.lhs = v;
    return eq;
  }
};

using NewEqs = std::vector<NewEq>;

/**
 * Branch/path condition atom tracked separately from phi together with its
 * interprocedural call depth (Infer's PulseFormula.conditions: int Atom.Map.t).
 *
 * depth == 0: condition tested directly in the current procedure.
 * depth > 0:  condition inherited from a callee summary.
 */
struct PathConditionAtom {
  enum class Kind {
    EqZero,       // lhs == 0
    NeZero,       // lhs != 0
    Equal,        // lhs == rhs (or lhs == const_rhs)
    NotEqual,     // lhs != rhs (or lhs != const_rhs)
    LessThan,     // lhs < rhs (or lhs < const_rhs)
    LessEqual,    // lhs <= rhs (or lhs <= const_rhs)
    GreaterThan,  // lhs > rhs (or lhs > const_rhs)
    GreaterEqual, // lhs >= rhs (or lhs >= const_rhs)
    Other
  };

  Kind kind{Kind::Other};
  AbstractValue lhs;
  AbstractValue rhs;
  std::optional<int64_t> const_rhs;
  bool is_ground{false};

  bool operator<(const PathConditionAtom &other) const {
    if (kind != other.kind)
      return kind < other.kind;
    if (!(lhs == other.lhs))
      return lhs < other.lhs;
    if (!(rhs == other.rhs))
      return rhs < other.rhs;
    if (const_rhs != other.const_rhs)
      return const_rhs < other.const_rhs;
    return is_ground < other.is_ground;
  }

  bool operator==(const PathConditionAtom &other) const {
    return kind == other.kind && lhs == other.lhs && rhs == other.rhs &&
           const_rhs == other.const_rhs && is_ground == other.is_ground;
  }

  static PathConditionAtom eqZero(AbstractValue v) {
    PathConditionAtom a;
    a.kind = Kind::EqZero;
    a.lhs = v;
    return a;
  }

  static PathConditionAtom neZero(AbstractValue v) {
    PathConditionAtom a;
    a.kind = Kind::NeZero;
    a.lhs = v;
    return a;
  }

  static PathConditionAtom makeRel(Kind k, AbstractValue l, AbstractValue r,
                                   std::optional<int64_t> c = std::nullopt) {
    PathConditionAtom a;
    a.kind = k;
    a.lhs = l;
    a.rhs = r;
    a.const_rhs = c;
    return a;
  }
};

/**
 * PulseFormula: tracks path conditions including equalities, disequalities,
 * nullness, bounds, linear/nonlinear arithmetic, and depth-annotated branch
 * conditions following Infer's PulseFormula design.
 */
class PulseFormula {
private:
  // Bottom flag: represents an unsatisfiable/contradictory path condition.
  bool is_contradiction_{false};
  // 0 = invalid, 1 = cached with SMT disabled, 2 = cached with SMT enabled.
  mutable uint8_t sat_cache_mode_{0};
  mutable bool sat_cache_result_{true};
  mutable bool consistent_cache_valid_{false};
  mutable bool consistent_cache_result_{true};
  void invalidateSatOnly() { sat_cache_mode_ = 0; }
  void invalidateSatCache() {
    sat_cache_mode_ = 0;
    consistent_cache_valid_ = false;
  }

  // Track equalities: v1 = v2 via union-find (each value -> representative).
  detail::FlatMap<AbstractValue, AbstractValue> equalities_;

  // Track disequalities: v1 != v2 (canonical pairs ordered (min, max)).
  detail::FlatSet<std::pair<AbstractValue, AbstractValue>> disequalities_;

  // Track non-null / non-zero constraints: value != 0.
  detail::FlatSet<AbstractValue> non_null_values_;

  // Track null / zero constraints: value == 0.
  detail::FlatSet<AbstractValue> null_values_;

  // Track integer constraints (linear arithmetic).
  std::vector<LinearConstraint> linear_constraints_;

  // Track integer bounds: lower_bound[v] <= v <= upper_bound[v].
  detail::FlatMap<AbstractValue, int64_t> lower_bounds_;
  detail::FlatMap<AbstractValue, int64_t> upper_bounds_;

  // Track if a value is known to be an integer.
  detail::FlatSet<AbstractValue> integer_values_;

  // Track arithmetic operations for Z3 encoding and summary propagation.
  struct ArithmeticOp {
    AbstractValue result;
    AbstractValue op1;
    AbstractValue op2;
    std::string op; // "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"
  };
  std::vector<ArithmeticOp> arithmetic_ops_;

  // Incremental Reduced-Row-Echelon (RREF) linear equality tableau:
  // pivot -> (sum(coeff_i * v_i) + constant == 0) with exact integer coeffs.
  struct LinearEqRow {
    detail::FlatMap<AbstractValue, int64_t> coeffs;
    int64_t constant{0};
  };
  detail::FlatMap<AbstractValue, LinearEqRow> linear_tableau_;
  bool in_tableau_propagate_{false};
  bool has_linear_inequalities_{false};

  // Type / InstanceOf predicates (Infer's PulseFormulaInstanceOf).
  detail::FlatMap<AbstractValue, std::string> exact_types_;
  std::map<AbstractValue, std::set<std::string>> instanceof_types_;
  std::map<AbstractValue, std::set<std::string>> not_instanceof_types_;

  // Floating-point constants, operations, and comparisons (Z3 Real theory +
  // exact constant folding).
  struct FloatOp {
    AbstractValue result;
    AbstractValue op1;
    AbstractValue op2;
    std::string op; // "+", "-", "*", "/"
  };
  struct FloatCmp {
    AbstractValue op1;
    AbstractValue op2;
    std::string pred; // "==", "!=", "<", "<=", ">", ">="
  };
  detail::FlatMap<AbstractValue, double> float_constants_;
  std::vector<FloatOp> float_ops_;
  std::vector<FloatCmp> float_cmps_;

  // Integer bit-widths (for width-aware constant folding / unsigned checks).
  detail::FlatMap<AbstractValue, unsigned> bit_widths_;

  // Disjunctive equalities produced by PulseJoin (v_join = lhs ∨ v_join = rhs).
  struct DisjunctiveEq {
    AbstractValue target;
    std::vector<AbstractValue> var_options;
    std::vector<int64_t> const_options;
  };
  std::vector<DisjunctiveEq> disjunctive_eqs_;

  // String & regexp predicates (Infer PulseFormulaTerm/Atom string theory).
  struct StringConcat {
    AbstractValue result;
    AbstractValue lhs;
    AbstractValue rhs;
  };
  struct StringRegexMatch {
    AbstractValue value;
    std::string pattern;
    bool positive{true};
  };
  detail::FlatMap<AbstractValue, std::string> string_constants_;
  detail::FlatMap<AbstractValue, AbstractValue> string_lengths_;
  std::vector<StringConcat> string_concats_;
  std::vector<StringRegexMatch> string_regex_matches_;

  // Branch conditions assumed along the path, mapped to minimum call depth.
  std::map<PathConditionAtom, unsigned> conditions_;

  // Pending new_eqs accumulated since last drain.
  NewEqs pending_new_eqs_;

  // Find canonical representative (with path compression). Mutates equalities_.
  AbstractValue findRep(AbstractValue v);

  // Follow equality chain without mutating.
  AbstractValue findRepReadOnly(AbstractValue v) const;

  static std::pair<AbstractValue, AbstractValue>
  normalizePair(AbstractValue v1, AbstractValue v2);

  PathConditionAtom normalizeConditionAtom(const PathConditionAtom &atom) const;

  bool isConditionImpliedByFacts(const PathConditionAtom &atom) const;

public:
  PulseFormula() = default;

  static PulseFormula contradiction() {
    PulseFormula f;
    f.is_contradiction_ = true;
    return f;
  }

  /**
   * Add equality constraint: v1 = v2.
   * Merges bounds, null/non-null, integer markers, and disequalities.
   * Returns true if consistent, false if contradicts existing constraints.
   */
  bool addEquality(AbstractValue v1, AbstractValue v2);

  /**
   * Add disequality constraint: v1 != v2.
   * Returns true if consistent, false if contradicts existing constraints.
   */
  bool addDisequality(AbstractValue v1, AbstractValue v2);

  /**
   * Add non-null constraint: value != 0.
   * Returns false if contradicts known null / zero bound.
   */
  bool addNonNull(AbstractValue v);

  /**
   * Add null constraint: value == 0.
   * Returns false if contradicts known non-null / non-zero bound.
   */
  bool addNull(AbstractValue v);

  /**
   * Record a branch/path condition assumed at the given interprocedural depth.
   */
  void recordCondition(const PathConditionAtom &atom, unsigned depth = 0);

  const std::map<PathConditionAtom, unsigned> &getConditions() const {
    return conditions_;
  }

  /**
   * Drain newly discovered equalities/zeros for AbductiveDomain::incorporateNewEqs.
   */
  NewEqs takeNewEqs() {
    NewEqs out;
    out.swap(pending_new_eqs_);
    return out;
  }

  const NewEqs &peekNewEqs() const { return pending_new_eqs_; }
  void clearNewEqs() { pending_new_eqs_.clear(); }

  AbstractValue getCanonical(AbstractValue v) const;
  AbstractValue getCanonicalReadOnly(AbstractValue v) const {
    return findRepReadOnly(v);
  }

  bool isNonNull(AbstractValue v) const;
  bool isNull(AbstractValue v) const;
  bool areEqual(AbstractValue v1, AbstractValue v2) const;
  bool areDisequal(AbstractValue v1, AbstractValue v2) const;

  /**
   * Check fast structural consistency (contradiction flag, null/non-null,
   * disequalities, and bounds).
   */
  bool isConsistent() const;

  bool isContradiction() const { return is_contradiction_; }
  bool hasEqualities() const { return !equalities_.empty(); }
  bool isEmpty() const {
    return !is_contradiction_ && equalities_.empty() &&
           disequalities_.empty() && non_null_values_.empty() &&
           null_values_.empty() && linear_constraints_.empty() &&
           lower_bounds_.empty() && upper_bounds_.empty() &&
           integer_values_.empty() && arithmetic_ops_.empty() &&
           linear_tableau_.empty() && exact_types_.empty() &&
           instanceof_types_.empty() && not_instanceof_types_.empty() &&
           float_constants_.empty() && float_ops_.empty() &&
           float_cmps_.empty() && bit_widths_.empty() &&
           disjunctive_eqs_.empty() && string_constants_.empty() &&
           string_lengths_.empty() && string_concats_.empty() &&
           string_regex_matches_.empty() && conditions_.empty();
  }

  bool hasNullConstraints() const;

  /**
   * Legacy state-level triviality check (no null constraints).
   */
  bool isEmptyOrTrivial() const;

  /**
   * Infer-aligned manifestness check (PulseFormula.is_manifest):
   * True iff every branch condition (at depth > 0, or all depths when
   * check_depth_zero is true) is either ground, `x != 0` with `is_allocated(x)`,
   * or `x != y` with both allocated.
   */
  bool isManifest(const std::function<bool(AbstractValue)> &is_allocated,
                  bool check_depth_zero = true) const;

  /**
   * Conjoin two formulas (f1 ∧ f2). Returns contradiction if inconsistent.
   */
  static PulseFormula merge(const PulseFormula &f1, const PulseFormula &f2);

  /**
   * Conjoin a callee summary formula into a caller formula (Infer's
   * Formula.and_callee_formula):
   * - Extends substitution with fresh caller variables for unmapped callee vars
   * - Increments callee condition depths by +1 (discharging facts already
   *   entailed by caller constants/depth-0 facts)
   * - Emits discovered caller NewEqs.
   */
  static std::optional<std::pair<PulseFormula, NewEqs>>
  andCalleeFormula(const PulseFormula &caller, const PulseFormula &callee,
                   Substitution &subst, AbstractValueFactory &factory);

  /**
   * Condition-guided over-approximating join (disjunction f1 ∨ f2), aligned
   * with Infer's PulseFormula.join: keeps facts over variables unrelated to
   * one-sided branch conditions and intersects shared constraints.
   */
  static PulseFormula join(const PulseFormula &f1, const PulseFormula &f2);

  PulseFormula clone() const;

  /**
   * Apply a callee-to-caller substitution to all tracked values, arithmetic
   * ops, bounds, linear constraints, and conditions.
   */
  PulseFormula applySubstitution(const Substitution &substitution,
                                 unsigned condition_depth_bump = 0) const;

  /**
   * Simplify and project formula for summary creation (Infer's Formula.simplify):
   * prefers `keep_vars` as canonical representatives, drops dead temporary
   * constraints, and returns any extra variables kept alive via arithmetic.
   */
  std::set<AbstractValue> simplifyForSummary(
      const std::set<AbstractValue> &keep_vars,
      const std::set<AbstractValue> &precondition_vocab = {},
      bool keep_non_ground_conditions = false);

  /**
   * Compare two formulas after canonical normalization.
   */
  bool equivalentTo(const PulseFormula &other) const;

  /**
   * Collect all abstract values mentioned in the formula.
   */
  std::set<AbstractValue> getReferencedValues() const;
  void collectReferencedValues(std::unordered_set<AbstractValue> &out) const;
  void collectConstrainedValues(llvm::DenseSet<AbstractValue> &out) const;
  bool hasValueConstraints(AbstractValue v) const;

  void addIntegerConstraint(AbstractValue v);
  bool addLinearConstraint(const LinearConstraint &constraint);
  bool addBounds(AbstractValue v, int64_t lower, int64_t upper);

  bool isInteger(AbstractValue v) const;
  std::optional<int64_t> getLowerBound(AbstractValue v) const;
  std::optional<int64_t> getUpperBound(AbstractValue v) const;
  std::optional<int64_t> asConstantInt(AbstractValue v) const;

  bool isUnsat() const;
  void simplify();

  /**
   * Query whether this formula entails `lhs <op> rhs` (optionally with constant
   * operands). Returns true if entailed, false if negated is entailed, or
   * std::nullopt if undecided.
   */
  std::optional<bool> impliesComparison(std::optional<AbstractValue> lhs_var,
                                        std::optional<int64_t> lhs_const,
                                        const std::string &op,
                                        std::optional<AbstractValue> rhs_var,
                                        std::optional<int64_t> rhs_const,
                                        bool allow_smt = true) const;

  bool addArithmeticOperation(AbstractValue result, AbstractValue op1,
                              AbstractValue op2, const std::string &op);
  bool addMultiply(AbstractValue result, AbstractValue op1, AbstractValue op2);
  bool addDivide(AbstractValue result, AbstractValue op1, AbstractValue op2);
  bool addModulo(AbstractValue result, AbstractValue op1, AbstractValue op2);
  bool addBitwiseAnd(AbstractValue result, AbstractValue op1,
                     AbstractValue op2);
  bool addBitwiseOr(AbstractValue result, AbstractValue op1, AbstractValue op2);
  bool addBitwiseXor(AbstractValue result, AbstractValue op1,
                     AbstractValue op2);
  bool addLeftShift(AbstractValue result, AbstractValue op1, AbstractValue op2);
  bool addRightShift(AbstractValue result, AbstractValue op1,
                     AbstractValue op2);

  // Type / InstanceOf predicates (Infer PulseFormulaInstanceOf) + class
  // inheritance hierarchy support (`Derived <: Base`).
  bool addExactType(AbstractValue v, const std::string &type_name);
  bool addInstanceOf(AbstractValue v, const std::string &type_name);
  bool addNotInstanceOf(AbstractValue v, const std::string &type_name);
  std::optional<std::string> getExactType(AbstractValue v) const;
  bool isInstanceOf(AbstractValue v, const std::string &type_name) const;

  static void registerSubtype(const std::string &derived_type,
                              const std::string &base_type);
  static bool isSubtypeOf(const std::string &derived_type,
                          const std::string &base_type);
  static std::set<std::string> getSupertypes(const std::string &type_name);
  static void clearSubtypeHierarchy();

  // String & regexp constraints.
  bool addStringConstant(AbstractValue v, const std::string &str);
  std::optional<std::string> asConstantString(AbstractValue v) const;
  bool addStringLength(AbstractValue str_v, AbstractValue len_v);
  bool addStringConcat(AbstractValue result, AbstractValue lhs,
                       AbstractValue rhs);
  bool addStringRegexMatch(AbstractValue v, const std::string &pattern,
                           bool positive = true);

  // Floating-point constraints and constants.
  bool addFloatConstant(AbstractValue v, double val);
  std::optional<double> asConstantFloat(AbstractValue v) const;
  bool addFloatOperation(AbstractValue result, AbstractValue op1,
                         AbstractValue op2, const std::string &op);
  bool addFloatComparison(AbstractValue op1, AbstractValue op2,
                          const std::string &pred);

  // Bit-width metadata.
  void setBitWidth(AbstractValue v, unsigned bits);
  std::optional<unsigned> getBitWidth(AbstractValue v) const;

  // Disjunctive equalities from PulseJoin (v_join = lhs ∨ v_join = rhs).
  bool addDisjunctiveEquality(AbstractValue target, AbstractValue opt1,
                              AbstractValue opt2,
                              std::optional<int64_t> c1 = std::nullopt,
                              std::optional<int64_t> c2 = std::nullopt);

  const std::vector<DisjunctiveEq> &getDisjunctiveEqs() const {
    return disjunctive_eqs_;
  }

private:
  bool checkSatisfiability() const;
  void propagateBounds();
  bool insertLinearEquationRow(LinearEqRow row);
  bool propagateLinearTableau();
  bool propagateLinearInequalities();
};

} // namespace pulse
