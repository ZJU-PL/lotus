/** @file PulseFormula.h @brief Symbolic formula representation for Pulse abstract values. */
#pragma once

#include "Checker/Pulse/Core/PulseAbstractValue.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pulse {

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

  // Track equalities: v1 = v2 via union-find (each value -> representative).
  std::map<AbstractValue, AbstractValue> equalities_;

  // Track disequalities: v1 != v2 (canonical pairs ordered (min, max)).
  std::set<std::pair<AbstractValue, AbstractValue>> disequalities_;

  // Track non-null / non-zero constraints: value != 0.
  std::set<AbstractValue> non_null_values_;

  // Track null / zero constraints: value == 0.
  std::set<AbstractValue> null_values_;

  // Track integer constraints (linear arithmetic).
  std::vector<LinearConstraint> linear_constraints_;

  // Track integer bounds: lower_bound[v] <= v <= upper_bound[v].
  std::map<AbstractValue, int64_t> lower_bounds_;
  std::map<AbstractValue, int64_t> upper_bounds_;

  // Track if a value is known to be an integer.
  std::set<AbstractValue> integer_values_;

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
    std::map<AbstractValue, int64_t> coeffs;
    int64_t constant{0};
  };
  std::map<AbstractValue, LinearEqRow> linear_tableau_;
  bool in_tableau_propagate_{false};

  // Type / InstanceOf predicates (Infer's PulseFormulaInstanceOf).
  std::map<AbstractValue, std::string> exact_types_;
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
  std::map<AbstractValue, double> float_constants_;
  std::vector<FloatOp> float_ops_;
  std::vector<FloatCmp> float_cmps_;

  // Integer bit-widths (for width-aware constant folding / unsigned checks).
  std::map<AbstractValue, unsigned> bit_widths_;

  // Disjunctive equalities produced by PulseJoin (v_join = lhs ∨ v_join = rhs).
  struct DisjunctiveEq {
    AbstractValue target;
    std::vector<AbstractValue> var_options;
    std::vector<int64_t> const_options;
  };
  std::vector<DisjunctiveEq> disjunctive_eqs_;

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
      const std::set<AbstractValue> &precondition_vocab = {});

  /**
   * Compare two formulas after canonical normalization.
   */
  bool equivalentTo(const PulseFormula &other) const;

  /**
   * Collect all abstract values mentioned in the formula.
   */
  std::set<AbstractValue> getReferencedValues() const;

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
                                        std::optional<int64_t> rhs_const) const;

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

  // Type / InstanceOf predicates (Infer PulseFormulaInstanceOf).
  bool addExactType(AbstractValue v, const std::string &type_name);
  bool addInstanceOf(AbstractValue v, const std::string &type_name);
  bool addNotInstanceOf(AbstractValue v, const std::string &type_name);
  std::optional<std::string> getExactType(AbstractValue v) const;

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
};

} // namespace pulse
