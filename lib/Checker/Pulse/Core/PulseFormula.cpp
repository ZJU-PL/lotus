#include "Checker/Pulse/Core/PulseFormula.h"

#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Domain/PulseOperations.h"
#include "Checker/Pulse/Report/PulseOptions.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

#include <z3++.h>

namespace pulse {

namespace {
// Single thread-local holder guaranteeing strict destruction order:
// expressions and solver are always destroyed BEFORE `ctx`.
struct Z3ThreadState {
  z3::context ctx;
  z3::solver simple_solver;
  std::vector<z3::expr> int_pool;
  std::vector<z3::expr> bv_pool;
  std::vector<z3::expr> real_pool;
  unsigned query_count{0};

  Z3ThreadState() : ctx(), simple_solver(ctx, z3::solver::simple()) {
    z3::params p(ctx);
    p.set(":timeout", 50u);
    simple_solver.set(p);
  }
};

Z3ThreadState &getZ3ThreadState() {
  static thread_local Z3ThreadState state;
  return state;
}

z3::context &getCachedZ3Context() { return getZ3ThreadState().ctx; }

// Persistent lightweight incremental SMT solver (`Z3_mk_simple_solver`),
// bypassing `tactic2solver` pipeline setup/teardown on every query.
// Periodically reset to keep learned clauses bounded.
z3::solver &getCachedSimpleSolver(z3::context &ctx) {
  auto &st = getZ3ThreadState();
  if (++st.query_count >= 512) {
    st.query_count = 0;
    st.simple_solver.reset();
    z3::params p(ctx);
    p.set(":timeout", 50u);
    st.simple_solver.set(p);
  }
  return st.simple_solver;
}

struct SolverPushScope {
  z3::solver &solver;
  explicit SolverPushScope(z3::solver &s) : solver(s) { solver.push(); }
  ~SolverPushScope() {
    try {
      solver.pop();
    } catch (...) {
    }
  }
};

// Pooled per-query normalized Z3 constants ("i0".."ik", "b0".."bk", "r0".."rk")
// so Z3's ast_manager never interns unbounded unique AbstractValue symbol names
// across an entire module.
z3::expr getPooledIntVar(z3::context &ctx, size_t idx) {
  auto &pool = getZ3ThreadState().int_pool;
  while (pool.size() <= idx) {
    std::string name = "i" + std::to_string(pool.size());
    pool.push_back(ctx.int_const(name.c_str()));
  }
  return pool[idx];
}

z3::expr getPooledBVVar(z3::context &ctx, size_t idx) {
  auto &pool = getZ3ThreadState().bv_pool;
  while (pool.size() <= idx) {
    std::string name = "b" + std::to_string(pool.size());
    pool.push_back(ctx.bv_const(name.c_str(), 64));
  }
  return pool[idx];
}

z3::expr getPooledRealVar(z3::context &ctx, size_t idx) {
  auto &pool = getZ3ThreadState().real_pool;
  while (pool.size() <= idx) {
    std::string name = "r" + std::to_string(pool.size());
    pool.push_back(ctx.real_const(name.c_str()));
  }
  return pool[idx];
}

std::map<std::string, std::set<std::string>> &subtypeHierarchyMap() {
  static std::map<std::string, std::set<std::string>> edges;
  return edges;
}

z3::expr toBV64(z3::context &ctx, const z3::expr &int_e) {
  return z3::expr(ctx, Z3_mk_int2bv(ctx, 64, int_e));
}

z3::expr fromBV64(z3::context &ctx, const z3::expr &bv_e) {
  return z3::expr(ctx, Z3_mk_bv2int(ctx, bv_e, /*is_signed=*/true));
}

z3::expr getZ3Var(z3::context &ctx, AbstractValue v,
                  std::unordered_map<AbstractValue, z3::expr> &var_map) {
  auto it = var_map.find(v);
  if (it != var_map.end()) {
    return it->second;
  }
  z3::expr var = getPooledIntVar(ctx, var_map.size());
  var_map.insert({v, var});
  return var;
}

z3::expr getZ3BVVar(z3::context &ctx, AbstractValue v,
                    std::unordered_map<AbstractValue, z3::expr> &var_map) {
  auto it = var_map.find(v);
  if (it != var_map.end()) {
    return it->second;
  }
  z3::expr var = getPooledBVVar(ctx, var_map.size());
  var_map.insert({v, var});
  return var;
}

z3::expr getZ3RealVar(z3::context &ctx, AbstractValue v,
                      std::unordered_map<AbstractValue, z3::expr> &var_map) {
  auto it = var_map.find(v);
  if (it != var_map.end()) {
    return it->second;
  }
  z3::expr var = getPooledRealVar(ctx, var_map.size());
  var_map.insert({v, var});
  return var;
}

z3::expr z3RealVal(z3::context &ctx, double d) {
  std::ostringstream oss;
  oss.precision(17);
  oss << std::fixed << d;
  return ctx.real_val(oss.str().c_str());
}

__int128 abs128(__int128 v) { return v < 0 ? -v : v; }

__int128 gcd128(__int128 a, __int128 b) {
  a = abs128(a);
  b = abs128(b);
  while (b != 0) {
    __int128 t = a % b;
    a = b;
    b = t;
  }
  return a == 0 ? 1 : a;
}

bool fitsInI64(__int128 v) {
  return v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()) &&
         v <= static_cast<__int128>(std::numeric_limits<int64_t>::max());
}

bool evalFloatCmp(const std::string &pred, double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return pred == "!=";
  }
  if (pred == "==")
    return a == b;
  if (pred == "!=")
    return a != b;
  if (pred == "<")
    return a < b;
  if (pred == "<=")
    return a <= b;
  if (pred == ">")
    return a > b;
  if (pred == ">=")
    return a >= b;
  return false;
}

int64_t clampToI64(__int128 v) {
  if (v > static_cast<__int128>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }
  if (v < static_cast<__int128>(std::numeric_limits<int64_t>::min())) {
    return std::numeric_limits<int64_t>::min();
  }
  return static_cast<int64_t>(v);
}

int64_t satAddI64(int64_t a, int64_t b) {
  return clampToI64(static_cast<__int128>(a) + static_cast<__int128>(b));
}

int64_t satMulI64(int64_t a, int64_t b) {
  return clampToI64(static_cast<__int128>(a) * static_cast<__int128>(b));
}

void collectConditionVars(const PathConditionAtom &atom,
                          std::set<AbstractValue> &vars) {
  if (atom.lhs.getId() != 0)
    vars.insert(atom.lhs);
  if (!atom.const_rhs && atom.rhs.getId() != 0)
    vars.insert(atom.rhs);
}

bool evalConcreteRel(PathConditionAtom::Kind kind, int64_t l, int64_t r) {
  switch (kind) {
  case PathConditionAtom::Kind::EqZero:
    return l == 0;
  case PathConditionAtom::Kind::NeZero:
    return l != 0;
  case PathConditionAtom::Kind::Equal:
    return l == r;
  case PathConditionAtom::Kind::NotEqual:
    return l != r;
  case PathConditionAtom::Kind::LessThan:
    return l < r;
  case PathConditionAtom::Kind::LessEqual:
    return l <= r;
  case PathConditionAtom::Kind::GreaterThan:
    return l > r;
  case PathConditionAtom::Kind::GreaterEqual:
    return l >= r;
  case PathConditionAtom::Kind::Other:
    return false;
  }
  return false;
}
} // namespace

AbstractValue PulseFormula::findRep(AbstractValue v) {
  std::vector<AbstractValue> path;
  AbstractValue cur = v;
  while (true) {
    auto it = equalities_.find(cur);
    if (it == equalities_.end() || it->second == cur) {
      break;
    }
    path.push_back(cur);
    cur = it->second;
  }
  AbstractValue rep = cur;
  for (AbstractValue u : path) {
    equalities_[u] = rep;
  }
  return rep;
}

AbstractValue PulseFormula::findRepReadOnly(AbstractValue v) const {
  AbstractValue cur = v;
  while (true) {
    auto it = equalities_.find(cur);
    if (it == equalities_.end() || it->second == cur) {
      return cur;
    }
    cur = it->second;
  }
}

std::pair<AbstractValue, AbstractValue>
PulseFormula::normalizePair(AbstractValue v1, AbstractValue v2) {
  if (v2 < v1) {
    std::swap(v1, v2);
  }
  return {v1, v2};
}

PathConditionAtom
PulseFormula::normalizeConditionAtom(const PathConditionAtom &atom) const {
  PathConditionAtom norm = atom;
  if (norm.lhs.getId() != 0) {
    norm.lhs = findRepReadOnly(norm.lhs);
  }
  if (!norm.const_rhs && norm.rhs.getId() != 0) {
    norm.rhs = findRepReadOnly(norm.rhs);
  }
  // Canonicalize symmetric binary predicates.
  if (!norm.const_rhs &&
      (norm.kind == PathConditionAtom::Kind::Equal ||
       norm.kind == PathConditionAtom::Kind::NotEqual) &&
      norm.rhs < norm.lhs) {
    std::swap(norm.lhs, norm.rhs);
  }
  return norm;
}

bool PulseFormula::isConditionImpliedByFacts(
    const PathConditionAtom &atom) const {
  PathConditionAtom norm = normalizeConditionAtom(atom);
  if (norm.is_ground) {
    auto l = asConstantInt(norm.lhs);
    auto r = norm.const_rhs ? norm.const_rhs : asConstantInt(norm.rhs);
    if (norm.kind == PathConditionAtom::Kind::EqZero ||
        norm.kind == PathConditionAtom::Kind::NeZero) {
      return l && evalConcreteRel(norm.kind, *l, 0);
    }
    if (l && r) {
      return evalConcreteRel(norm.kind, *l, *r);
    }
  }
  switch (norm.kind) {
  case PathConditionAtom::Kind::EqZero:
    return isNull(norm.lhs);
  case PathConditionAtom::Kind::NeZero:
    return isNonNull(norm.lhs);
  case PathConditionAtom::Kind::Equal:
    if (norm.const_rhs) {
      auto c = asConstantInt(norm.lhs);
      return c && *c == *norm.const_rhs;
    }
    return areEqual(norm.lhs, norm.rhs);
  case PathConditionAtom::Kind::NotEqual:
    if (norm.const_rhs) {
      if (*norm.const_rhs == 0 && isNonNull(norm.lhs))
        return true;
      auto lo = getLowerBound(norm.lhs);
      auto hi = getUpperBound(norm.lhs);
      if ((hi && *hi < *norm.const_rhs) || (lo && *lo > *norm.const_rhs))
        return true;
      return false;
    }
    return areDisequal(norm.lhs, norm.rhs);
  case PathConditionAtom::Kind::LessThan:
  case PathConditionAtom::Kind::LessEqual:
  case PathConditionAtom::Kind::GreaterThan:
  case PathConditionAtom::Kind::GreaterEqual: {
    std::string op;
    if (norm.kind == PathConditionAtom::Kind::LessThan)
      op = "<";
    else if (norm.kind == PathConditionAtom::Kind::LessEqual)
      op = "<=";
    else if (norm.kind == PathConditionAtom::Kind::GreaterThan)
      op = ">";
    else
      op = ">=";
    auto res = impliesComparison(norm.lhs, std::nullopt, op,
                                 norm.const_rhs ? std::nullopt
                                                : std::optional<AbstractValue>(norm.rhs),
                                 norm.const_rhs, /*allow_smt=*/false);
    return res.has_value() && *res;
  }
  case PathConditionAtom::Kind::Other:
    return false;
  }
  return false;
}

bool PulseFormula::addEquality(AbstractValue v1, AbstractValue v2) {
  if (is_contradiction_) {
    return false;
  }
  if (v1 == v2) {
    return true;
  }

  AbstractValue rep1 = findRep(v1);
  AbstractValue rep2 = findRep(v2);

  if (rep1 == rep2) {
    return true;
  }

  if (disequalities_.count(normalizePair(rep1, rep2)) > 0) {
    return false;
  }

  // Check for contradictions with null/non-null.
  const bool rep1_null = null_values_.count(rep1) > 0;
  const bool rep2_null = null_values_.count(rep2) > 0;
  const bool rep1_non_null = non_null_values_.count(rep1) > 0;
  const bool rep2_non_null = non_null_values_.count(rep2) > 0;
  if ((rep1_null && rep2_non_null) || (rep1_non_null && rep2_null)) {
    return false;
  }

  // Check merged bounds before committing the union.
  std::optional<int64_t> merged_lo;
  std::optional<int64_t> merged_hi;
  auto lo1 = lower_bounds_.find(rep1);
  auto lo2 = lower_bounds_.find(rep2);
  if (lo1 != lower_bounds_.end() && lo2 != lower_bounds_.end()) {
    merged_lo = std::max(lo1->second, lo2->second);
  } else if (lo1 != lower_bounds_.end()) {
    merged_lo = lo1->second;
  } else if (lo2 != lower_bounds_.end()) {
    merged_lo = lo2->second;
  }

  auto hi1 = upper_bounds_.find(rep1);
  auto hi2 = upper_bounds_.find(rep2);
  if (hi1 != upper_bounds_.end() && hi2 != upper_bounds_.end()) {
    merged_hi = std::min(hi1->second, hi2->second);
  } else if (hi1 != upper_bounds_.end()) {
    merged_hi = hi1->second;
  } else if (hi2 != upper_bounds_.end()) {
    merged_hi = hi2->second;
  }

  if (merged_lo && merged_hi && *merged_lo > *merged_hi) {
    return false;
  }
  const bool merged_null = rep1_null || rep2_null;
  const bool merged_non_null = rep1_non_null || rep2_non_null;
  if (merged_null &&
      ((merged_lo && *merged_lo > 0) || (merged_hi && *merged_hi < 0))) {
    return false;
  }
  if (merged_non_null && merged_lo && merged_hi && *merged_lo == 0 &&
      *merged_hi == 0) {
    return false;
  }

  // Check Type / InstanceOf compatibility before committing the union.
  auto et1 = exact_types_.find(rep1);
  auto et2 = exact_types_.find(rep2);
  if (et1 != exact_types_.end() && et2 != exact_types_.end() &&
      et1->second != et2->second) {
    return false;
  }
  std::optional<std::string> merged_exact;
  if (et1 != exact_types_.end())
    merged_exact = et1->second;
  else if (et2 != exact_types_.end())
    merged_exact = et2->second;

  auto io1 = instanceof_types_.find(rep1);
  auto io2 = instanceof_types_.find(rep2);
  auto nio1 = not_instanceof_types_.find(rep1);
  auto nio2 = not_instanceof_types_.find(rep2);
  auto hasNotType = [&](const std::string &t) {
    auto checkMap = [&](const auto &m_it) {
      if (m_it == not_instanceof_types_.end())
        return false;
      for (const auto &neg_t : m_it->second) {
        if (isSubtypeOf(t, neg_t))
          return true;
      }
      return false;
    };
    return checkMap(nio1) || checkMap(nio2);
  };
  if (merged_exact && hasNotType(*merged_exact)) {
    return false;
  }
  if (io1 != instanceof_types_.end()) {
    for (const auto &t : io1->second) {
      if (hasNotType(t))
        return false;
    }
  }
  if (io2 != instanceof_types_.end()) {
    for (const auto &t : io2->second) {
      if (hasNotType(t))
        return false;
    }
  }

  // Check Float & String constant compatibility before committing the union.
  auto fc1 = float_constants_.find(rep1);
  auto fc2 = float_constants_.find(rep2);
  if (fc1 != float_constants_.end() && fc2 != float_constants_.end() &&
      fc1->second != fc2->second) {
    return false;
  }
  auto sc1 = string_constants_.find(rep1);
  auto sc2 = string_constants_.find(rep2);
  if (sc1 != string_constants_.end() && sc2 != string_constants_.end() &&
      sc1->second != sc2->second) {
    return false;
  }

  // Merge: make rep1 the canonical representative.
  invalidateSatCache();
  equalities_[rep2] = rep1;
  pending_new_eqs_.push_back(NewEq::makeEqual(rep2, rep1));

  // Merge Type / InstanceOf / Float / String / BitWidth metadata onto rep1.
  if (et2 != exact_types_.end()) {
    exact_types_[rep1] = et2->second;
    exact_types_.erase(rep2);
  }
  if (io2 != instanceof_types_.end()) {
    instanceof_types_[rep1].insert(io2->second.begin(), io2->second.end());
    instanceof_types_.erase(rep2);
  }
  if (nio2 != not_instanceof_types_.end()) {
    not_instanceof_types_[rep1].insert(nio2->second.begin(),
                                       nio2->second.end());
    not_instanceof_types_.erase(rep2);
  }
  if (fc2 != float_constants_.end()) {
    float_constants_[rep1] = fc2->second;
    float_constants_.erase(rep2);
  }
  if (sc2 != string_constants_.end()) {
    string_constants_[rep1] = sc2->second;
    string_constants_.erase(rep2);
  }
  auto sl2 = string_lengths_.find(rep2);
  if (sl2 != string_lengths_.end()) {
    AbstractValue len2 = sl2->second;
    string_lengths_.erase(rep2);
    auto sl1 = string_lengths_.find(rep1);
    if (sl1 == string_lengths_.end()) {
      string_lengths_[rep1] = len2;
    } else if (!addEquality(sl1->second, len2)) {
      return false;
    }
  }
  auto bw2 = bit_widths_.find(rep2);
  if (bw2 != bit_widths_.end()) {
    if (bit_widths_.find(rep1) == bit_widths_.end()) {
      bit_widths_[rep1] = bw2->second;
    }
    bit_widths_.erase(rep2);
  }

  // Merge null/non-null sets.
  if (rep2_null) {
    null_values_.erase(rep2);
    null_values_.insert(rep1);
    if (!rep1_null) {
      pending_new_eqs_.push_back(NewEq::makeEqZero(rep1));
    }
  }
  if (rep2_non_null) {
    non_null_values_.erase(rep2);
    non_null_values_.insert(rep1);
  }

  // Merge integer markers.
  if (integer_values_.count(rep2) > 0) {
    integer_values_.erase(rep2);
    integer_values_.insert(rep1);
  }

  // Merge bounds onto rep1.
  lower_bounds_.erase(rep2);
  upper_bounds_.erase(rep2);
  if (merged_lo) {
    lower_bounds_[rep1] = *merged_lo;
  }
  if (merged_hi) {
    upper_bounds_[rep1] = *merged_hi;
  }

  // Propagate exact zero / non-zero implied by bounds.
  if (merged_lo && merged_hi && *merged_lo == 0 && *merged_hi == 0) {
    if (null_values_.insert(rep1).second) {
      pending_new_eqs_.push_back(NewEq::makeEqZero(rep1));
    }
  } else if ((merged_lo && *merged_lo > 0) || (merged_hi && *merged_hi < 0)) {
    non_null_values_.insert(rep1);
  }

  // Re-normalize disequalities and check for violation.
  if (!disequalities_.empty()) {
    std::set<std::pair<AbstractValue, AbstractValue>> updated;
    for (const auto &p : disequalities_) {
      AbstractValue a = (p.first == rep2) ? rep1 : findRep(p.first);
      AbstractValue b = (p.second == rep2) ? rep1 : findRep(p.second);
      if (a == b) {
        return false;
      }
      updated.insert(normalizePair(a, b));
    }
    disequalities_.swap(updated);
  }

  // Re-normalize stored conditions.
  if (!conditions_.empty()) {
    std::map<PathConditionAtom, unsigned> updated_conds;
    for (const auto &kv : conditions_) {
      PathConditionAtom norm = normalizeConditionAtom(kv.first);
      auto it = updated_conds.find(norm);
      if (it == updated_conds.end()) {
        updated_conds.emplace(std::move(norm), kv.second);
      } else {
        it->second = std::min(it->second, kv.second);
      }
    }
    conditions_.swap(updated_conds);
  }

  if (!in_tableau_propagate_ && !linear_tableau_.empty()) {
    if (!propagateLinearTableau()) {
      return false;
    }
  }
  if (!in_tableau_propagate_ && has_linear_inequalities_ &&
      !linear_constraints_.empty()) {
    if (!propagateLinearInequalities()) {
      return false;
    }
  }

  return true;
}

bool PulseFormula::addDisequality(AbstractValue v1, AbstractValue v2) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue rep1 = findRep(v1);
  AbstractValue rep2 = findRep(v2);
  if (rep1 == rep2) {
    return false;
  }
  if (null_values_.count(rep1) > 0 && null_values_.count(rep2) > 0) {
    return false; // Both are 0 -> cannot be disequal.
  }
  auto c1 = asConstantInt(rep1);
  auto c2 = asConstantInt(rep2);
  if (c1 && c2 && *c1 == *c2) {
    return false;
  }
  if (null_values_.count(rep1) > 0) {
    if (!addNonNull(rep2))
      return false;
  } else if (null_values_.count(rep2) > 0) {
    if (!addNonNull(rep1))
      return false;
  }
  invalidateSatCache();
  disequalities_.insert(normalizePair(rep1, rep2));
  return true;
}

bool PulseFormula::addNonNull(AbstractValue v) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue rep = findRep(v);
  if (null_values_.count(rep) > 0) {
    return false; // Contradiction: already known null/zero.
  }
  auto lo = lower_bounds_.find(rep);
  auto hi = upper_bounds_.find(rep);
  if (lo != lower_bounds_.end() && hi != upper_bounds_.end() &&
      lo->second == 0 && hi->second == 0) {
    return false; // Contradiction: value is concretely 0.
  }
  if (non_null_values_.insert(rep).second) {
    invalidateSatCache();
  }
  return true;
}

bool PulseFormula::addNull(AbstractValue v) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue rep = findRep(v);
  if (non_null_values_.count(rep) > 0) {
    return false; // Contradiction: already non-null.
  }
  auto lo = lower_bounds_.find(rep);
  auto hi = upper_bounds_.find(rep);
  if ((lo != lower_bounds_.end() && lo->second > 0) ||
      (hi != upper_bounds_.end() && hi->second < 0)) {
    return false; // Contradiction: 0 excluded by bounds.
  }
  // Check if any disequal value is also null (0 != 0 is impossible).
  for (const auto &p : disequalities_) {
    AbstractValue a = findRep(p.first);
    AbstractValue b = findRep(p.second);
    if ((a == rep && null_values_.count(b) > 0) ||
        (b == rep && null_values_.count(a) > 0)) {
      return false;
    }
  }
  if (null_values_.insert(rep).second) {
    invalidateSatCache();
    pending_new_eqs_.push_back(NewEq::makeEqZero(rep));
    if (!in_tableau_propagate_ && !linear_tableau_.empty()) {
      if (!propagateLinearTableau()) {
        return false;
      }
    }
  }
  return true;
}

void PulseFormula::recordCondition(const PathConditionAtom &atom,
                                   unsigned depth) {
  if (is_contradiction_) {
    return;
  }
  PathConditionAtom norm = normalizeConditionAtom(atom);
  auto it = conditions_.find(norm);
  if (it == conditions_.end()) {
    conditions_.emplace(std::move(norm), depth);
  } else {
    it->second = std::min(it->second, depth);
  }
}

AbstractValue PulseFormula::getCanonical(AbstractValue v) const {
  return const_cast<PulseFormula *>(this)->findRep(v);
}

bool PulseFormula::isNonNull(AbstractValue v) const {
  AbstractValue rep = findRepReadOnly(v);
  if (non_null_values_.count(rep) > 0) {
    return true;
  }
  auto lo = lower_bounds_.find(rep);
  auto hi = upper_bounds_.find(rep);
  return (lo != lower_bounds_.end() && lo->second > 0) ||
         (hi != upper_bounds_.end() && hi->second < 0);
}

bool PulseFormula::isNull(AbstractValue v) const {
  AbstractValue rep = findRepReadOnly(v);
  if (null_values_.count(rep) > 0) {
    return true;
  }
  auto lo = lower_bounds_.find(rep);
  auto hi = upper_bounds_.find(rep);
  return lo != lower_bounds_.end() && hi != upper_bounds_.end() &&
         lo->second == 0 && hi->second == 0;
}

bool PulseFormula::areEqual(AbstractValue v1, AbstractValue v2) const {
  if (v1 == v2) {
    return true;
  }
  AbstractValue rep1 = findRepReadOnly(v1);
  AbstractValue rep2 = findRepReadOnly(v2);
  if (rep1 == rep2) {
    return true;
  }
  auto c1 = asConstantInt(rep1);
  auto c2 = asConstantInt(rep2);
  if (c1 && c2 && *c1 == *c2) {
    return true;
  }
  return false;
}

bool PulseFormula::areDisequal(AbstractValue v1, AbstractValue v2) const {
  AbstractValue rep1 = findRepReadOnly(v1);
  AbstractValue rep2 = findRepReadOnly(v2);
  if (rep1 == rep2) {
    return false;
  }
  if (disequalities_.count(normalizePair(rep1, rep2)) > 0) {
    return true;
  }
  if ((isNull(rep1) && isNonNull(rep2)) || (isNonNull(rep1) && isNull(rep2))) {
    return true;
  }
  auto lo1 = getLowerBound(rep1);
  auto hi1 = getUpperBound(rep1);
  auto lo2 = getLowerBound(rep2);
  auto hi2 = getUpperBound(rep2);
  if ((hi1 && lo2 && *hi1 < *lo2) || (hi2 && lo1 && *hi2 < *lo1)) {
    return true;
  }
  return false;
}

bool PulseFormula::isConsistent() const {
  if (is_contradiction_) {
    return false;
  }
  if (consistent_cache_valid_) {
    return consistent_cache_result_;
  }
  auto cacheConsistent = [&](bool res) {
    consistent_cache_valid_ = true;
    consistent_cache_result_ = res;
    return res;
  };
  for (AbstractValue null_val : null_values_) {
    AbstractValue rep = findRepReadOnly(null_val);
    if (non_null_values_.count(rep) > 0) {
      return cacheConsistent(false);
    }
    auto lo = lower_bounds_.find(rep);
    auto hi = upper_bounds_.find(rep);
    if ((lo != lower_bounds_.end() && lo->second > 0) ||
        (hi != upper_bounds_.end() && hi->second < 0)) {
      return cacheConsistent(false);
    }
  }
  for (AbstractValue non_null_val : non_null_values_) {
    AbstractValue rep = findRepReadOnly(non_null_val);
    auto lo = lower_bounds_.find(rep);
    auto hi = upper_bounds_.find(rep);
    if (lo != lower_bounds_.end() && hi != upper_bounds_.end() &&
        lo->second == 0 && hi->second == 0) {
      return cacheConsistent(false);
    }
  }
  for (const auto &kv : lower_bounds_) {
    AbstractValue rep = findRepReadOnly(kv.first);
    auto hi = upper_bounds_.find(rep);
    if (hi != upper_bounds_.end() && kv.second > hi->second) {
      return cacheConsistent(false);
    }
  }
  for (const auto &p : disequalities_) {
    AbstractValue rep1 = findRepReadOnly(p.first);
    AbstractValue rep2 = findRepReadOnly(p.second);
    if (rep1 == rep2) {
      return cacheConsistent(false);
    }
    if (null_values_.count(rep1) > 0 && null_values_.count(rep2) > 0) {
      return cacheConsistent(false);
    }
  }
  for (const auto &kv : exact_types_) {
    AbstractValue rep = findRepReadOnly(kv.first);
    auto nio = not_instanceof_types_.find(rep);
    if (nio != not_instanceof_types_.end()) {
      for (const auto &neg_t : nio->second) {
        if (isSubtypeOf(kv.second, neg_t)) {
          return cacheConsistent(false);
        }
      }
    }
  }
  for (const auto &kv : instanceof_types_) {
    AbstractValue rep = findRepReadOnly(kv.first);
    auto nio = not_instanceof_types_.find(rep);
    if (nio != not_instanceof_types_.end()) {
      for (const auto &t : kv.second) {
        for (const auto &neg_t : nio->second) {
          if (isSubtypeOf(t, neg_t)) {
            return cacheConsistent(false);
          }
        }
      }
    }
  }
  for (const auto &fc : float_cmps_) {
    auto a = asConstantFloat(fc.op1);
    auto b = asConstantFloat(fc.op2);
    if (a && b && !evalFloatCmp(fc.pred, *a, *b)) {
      return cacheConsistent(false);
    }
  }
  for (const auto &rm : string_regex_matches_) {
    if (auto s = asConstantString(rm.value)) {
      try {
        bool hit = std::regex_search(*s, std::regex(rm.pattern));
        if (hit != rm.positive) {
          return cacheConsistent(false);
        }
      } catch (const std::regex_error &) {
      }
    }
  }
  return cacheConsistent(true);
}

bool PulseFormula::hasNullConstraints() const { return !null_values_.empty(); }

bool PulseFormula::isEmptyOrTrivial() const {
  if (!isConsistent())
    return false;
  return !hasNullConstraints();
}

bool PulseFormula::isManifest(
    const std::function<bool(AbstractValue)> &is_allocated,
    bool check_depth_zero) const {
  if (!isConsistent()) {
    return false;
  }
  for (const auto &kv : conditions_) {
    const PathConditionAtom &cond = kv.first;
    const unsigned depth = kv.second;
    if (cond.is_ground || (!check_depth_zero && depth == 0)) {
      continue;
    }
    if (cond.kind == PathConditionAtom::Kind::NeZero &&
        is_allocated(findRepReadOnly(cond.lhs))) {
      continue;
    }
    if (cond.kind == PathConditionAtom::Kind::NotEqual && !cond.const_rhs &&
        is_allocated(findRepReadOnly(cond.lhs)) &&
        is_allocated(findRepReadOnly(cond.rhs))) {
      continue;
    }
    if (cond.kind == PathConditionAtom::Kind::NotEqual && cond.const_rhs &&
        *cond.const_rhs == 0 && is_allocated(findRepReadOnly(cond.lhs))) {
      continue;
    }
    return false;
  }
  return true;
}

PulseFormula PulseFormula::merge(const PulseFormula &f1,
                                 const PulseFormula &f2) {
  if (f1.is_contradiction_ || f2.is_contradiction_) {
    return PulseFormula::contradiction();
  }
  if (f2.isEmpty()) {
    return f1.clone();
  }
  if (f1.isEmpty()) {
    return f2.clone();
  }
  PulseFormula result = f1.clone();
  // Defer tableau and difference-constraint propagation until all f2 facts
  // are inserted so bulk merging is O(N) rather than O(N^2)/O(N^3).
  result.in_tableau_propagate_ = true;

  // Add equalities from f2.
  for (const auto &eq : f2.equalities_) {
    if (!result.addEquality(eq.first, eq.second)) {
      return PulseFormula::contradiction();
    }
  }

  // Add disequalities from f2.
  for (const auto &neq : f2.disequalities_) {
    if (!result.addDisequality(neq.first, neq.second)) {
      return PulseFormula::contradiction();
    }
  }

  // Add non-null constraints from f2 (checking for contradictions).
  for (AbstractValue v : f2.non_null_values_) {
    if (!result.addNonNull(v)) {
      return PulseFormula::contradiction();
    }
  }

  // Add null constraints from f2 (checking for contradictions).
  for (AbstractValue v : f2.null_values_) {
    if (!result.addNull(v)) {
      return PulseFormula::contradiction();
    }
  }

  // Merge bounds using canonical representatives (intersection).
  for (const auto &kv : f2.lower_bounds_) {
    int64_t hi = std::numeric_limits<int64_t>::max();
    auto hi_it = f2.upper_bounds_.find(kv.first);
    if (hi_it != f2.upper_bounds_.end()) {
      hi = hi_it->second;
    }
    if (!result.addBounds(kv.first, kv.second, hi)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &kv : f2.upper_bounds_) {
    if (f2.lower_bounds_.count(kv.first) > 0) {
      continue; // Already merged above.
    }
    if (!result.addBounds(kv.first, std::numeric_limits<int64_t>::min(),
                          kv.second)) {
      return PulseFormula::contradiction();
    }
  }

  // Merge integer markers.
  for (AbstractValue v : f2.integer_values_) {
    result.addIntegerConstraint(v);
  }

  // Merge linear constraints.
  for (const auto &constraint : f2.linear_constraints_) {
    if (!result.addLinearConstraint(constraint)) {
      return PulseFormula::contradiction();
    }
  }

  // Merge arithmetic operations (previously lost!).
  for (const auto &op : f2.arithmetic_ops_) {
    if (!result.addArithmeticOperation(op.result, op.op1, op.op2, op.op)) {
      return PulseFormula::contradiction();
    }
  }

  // Merge Type / InstanceOf predicates.
  for (const auto &kv : f2.exact_types_) {
    if (!result.addExactType(kv.first, kv.second)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &kv : f2.instanceof_types_) {
    for (const auto &t : kv.second) {
      if (!result.addInstanceOf(kv.first, t)) {
        return PulseFormula::contradiction();
      }
    }
  }
  for (const auto &kv : f2.not_instanceof_types_) {
    for (const auto &t : kv.second) {
      if (!result.addNotInstanceOf(kv.first, t)) {
        return PulseFormula::contradiction();
      }
    }
  }

  // Merge Float constants, operations, comparisons, bit-widths, disjunctive eqs.
  for (const auto &kv : f2.float_constants_) {
    if (!result.addFloatConstant(kv.first, kv.second)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &fop : f2.float_ops_) {
    if (!result.addFloatOperation(fop.result, fop.op1, fop.op2, fop.op)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &fcmp : f2.float_cmps_) {
    if (!result.addFloatComparison(fcmp.op1, fcmp.op2, fcmp.pred)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &kv : f2.bit_widths_) {
    result.setBitWidth(kv.first, kv.second);
  }
  constexpr size_t kMaxMergedDisjEqs = 64;
  for (const auto &deq : f2.disjunctive_eqs_) {
    if (result.disjunctive_eqs_.size() >= kMaxMergedDisjEqs)
      break;
    DisjunctiveEq copy = deq;
    copy.target = result.findRep(copy.target);
    for (auto &v : copy.var_options) {
      v = result.findRep(v);
    }
    result.disjunctive_eqs_.push_back(std::move(copy));
  }
  for (const auto &kv : f2.string_constants_) {
    if (!result.addStringConstant(kv.first, kv.second)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &kv : f2.string_lengths_) {
    if (!result.addStringLength(kv.first, kv.second)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &sc : f2.string_concats_) {
    if (!result.addStringConcat(sc.result, sc.lhs, sc.rhs)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &rm : f2.string_regex_matches_) {
    if (!result.addStringRegexMatch(rm.value, rm.pattern, rm.positive)) {
      return PulseFormula::contradiction();
    }
  }

  // Merge path condition atoms (preserving minimum call depth).
  for (const auto &kv : f2.conditions_) {
    result.recordCondition(kv.first, kv.second);
  }

  result.in_tableau_propagate_ = false;
  if (!result.linear_tableau_.empty()) {
    if (!result.propagateLinearTableau()) {
      return PulseFormula::contradiction();
    }
  }
  if (result.has_linear_inequalities_ && !result.linear_constraints_.empty()) {
    if (!result.propagateLinearInequalities()) {
      return PulseFormula::contradiction();
    }
  }

  if (!result.isConsistent()) {
    return PulseFormula::contradiction();
  }

  // Preserve f1's Z3 SAT result when f2 adds only pure unary/eq/diseq facts
  // completely disjoint from f1's relational/arithmetic/disjunctive variables
  // (avoids redundant Z3 re-solving at every callee summary application).
  if (f1.sat_cache_mode_ == 2u && f1.sat_cache_result_ &&
      f2.linear_constraints_.empty() && f2.arithmetic_ops_.empty() &&
      f2.float_ops_.empty() && f2.float_cmps_.empty() &&
      f2.disjunctive_eqs_.empty()) {
    std::set<AbstractValue> f1_rel_vars;
    for (const auto &lc : f1.linear_constraints_) {
      for (const auto &t : lc.terms)
        f1_rel_vars.insert(f1.findRepReadOnly(t.var));
    }
    for (const auto &op : f1.arithmetic_ops_) {
      f1_rel_vars.insert(f1.findRepReadOnly(op.result));
      f1_rel_vars.insert(f1.findRepReadOnly(op.op1));
      f1_rel_vars.insert(f1.findRepReadOnly(op.op2));
    }
    for (const auto &deq : f1.disjunctive_eqs_) {
      f1_rel_vars.insert(f1.findRepReadOnly(deq.target));
      for (AbstractValue ov : deq.var_options)
        f1_rel_vars.insert(f1.findRepReadOnly(ov));
    }
    for (const auto &neq : f1.disequalities_) {
      f1_rel_vars.insert(f1.findRepReadOnly(neq.first));
      f1_rel_vars.insert(f1.findRepReadOnly(neq.second));
    }
    bool touches = false;
    auto checkTouch = [&](AbstractValue v) {
      if (f1_rel_vars.count(f1.findRepReadOnly(v)) > 0)
        touches = true;
    };
    for (const auto &eq : f2.equalities_) {
      checkTouch(eq.first);
      checkTouch(eq.second);
      if (touches)
        break;
    }
    if (!touches) {
      for (const auto &neq : f2.disequalities_) {
        checkTouch(neq.first);
        checkTouch(neq.second);
        if (touches)
          break;
      }
    }
    if (!touches) {
      for (AbstractValue v : f2.null_values_) {
        checkTouch(v);
        if (touches)
          break;
      }
    }
    if (!touches) {
      for (AbstractValue v : f2.non_null_values_) {
        checkTouch(v);
        if (touches)
          break;
      }
    }
    if (!touches) {
      for (const auto &kv : f2.lower_bounds_) {
        checkTouch(kv.first);
        if (touches)
          break;
      }
    }
    if (!touches) {
      for (const auto &kv : f2.upper_bounds_) {
        checkTouch(kv.first);
        if (touches)
          break;
      }
    }
    if (!touches) {
      result.sat_cache_mode_ = 2u;
      result.sat_cache_result_ = true;
    }
  }

  return result;
}

std::optional<std::pair<PulseFormula, NewEqs>>
PulseFormula::andCalleeFormula(const PulseFormula &caller,
                               const PulseFormula &callee, Substitution &subst,
                               AbstractValueFactory &factory) {
  if (caller.is_contradiction_ || callee.is_contradiction_) {
    return std::nullopt;
  }

  // Ensure every callee variable referenced in callee formula has a caller
  // mapping (creating fresh caller variables for unmapped callee temporaries).
  std::unordered_set<AbstractValue> callee_vars;
  callee.collectReferencedValues(callee_vars);
  for (AbstractValue cv : callee_vars) {
    AbstractValue canon_cv = callee.findRepReadOnly(cv);
    if (!subst.substitute(canon_cv)) {
      AbstractValue fresh = factory.createFresh();
      subst.add(canon_cv, fresh);
    }
    if (!(cv == canon_cv) && !subst.substitute(cv)) {
      subst.add(cv, *subst.substitute(canon_cv));
    }
  }

  // Substitute callee formula into caller space, bumping condition depth by +1.
  PulseFormula substituted_callee = callee.applySubstitution(subst, 1u);
  if (substituted_callee.isContradiction() ||
      !substituted_callee.isConsistent()) {
    return std::nullopt;
  }

  // Before merging conditions, discharge any callee condition already implied
  // by the caller's concrete facts (not merely another branch assumption).
  std::map<PathConditionAtom, unsigned> adjusted_conds;
  for (const auto &kv : substituted_callee.conditions_) {
    PathConditionAtom norm = caller.normalizeConditionAtom(kv.first);
    unsigned depth = kv.second;
    if (norm.is_ground) {
      continue;
    }
    if (caller.isConditionImpliedByFacts(norm)) {
      auto caller_it = caller.conditions_.find(norm);
      if (caller_it == caller.conditions_.end()) {
        // Entailed by concrete caller value (e.g., null constant or integer
        // literal): discharge this condition completely!
        continue;
      }
      depth = caller_it->second;
    }
    auto it = adjusted_conds.find(norm);
    if (it == adjusted_conds.end()) {
      adjusted_conds.emplace(std::move(norm), depth);
    } else {
      it->second = std::min(it->second, depth);
    }
  }
  substituted_callee.conditions_.swap(adjusted_conds);

  const size_t caller_existing_eqs = caller.peekNewEqs().size();
  PulseFormula merged = PulseFormula::merge(caller, substituted_callee);
  if (merged.isContradiction() || !merged.isConsistent() || merged.isUnsat()) {
    return std::nullopt;
  }

  NewEqs new_eqs = merged.takeNewEqs();
  if (caller_existing_eqs > 0 && new_eqs.size() >= caller_existing_eqs) {
    new_eqs.erase(new_eqs.begin(),
                  new_eqs.begin() +
                      static_cast<std::ptrdiff_t>(caller_existing_eqs));
  }
  return std::make_pair(std::move(merged), std::move(new_eqs));
}

PulseFormula PulseFormula::join(const PulseFormula &f1,
                                const PulseFormula &f2) {
  // f1 ∨ f2: if one side is UNSAT, return the other side.
  if (f1.is_contradiction_) {
    return f2.clone();
  }
  if (f2.is_contradiction_) {
    return f1.clone();
  }

  PulseFormula out;
  out.in_tableau_propagate_ = true;

  // Infer-style condition-guided join (PulseFormula.ml:1317-1363):
  // 1. Intersect branch conditions, keeping min depth.
  // 2. Collect variables mentioned in one-sided (killed) branch conditions.
  // 3. Keep facts over variables NOT tainted by killed conditions, plus facts
  //    that hold identically on both sides.
  std::set<AbstractValue> killed_lhs;
  std::set<AbstractValue> killed_rhs;
  std::map<PathConditionAtom, unsigned> joined_conditions;

  for (const auto &kv : f1.conditions_) {
    auto it2 = f2.conditions_.find(kv.first);
    if (it2 != f2.conditions_.end()) {
      joined_conditions[kv.first] = std::min(kv.second, it2->second);
    } else {
      collectConditionVars(kv.first, killed_lhs);
    }
  }
  for (const auto &kv : f2.conditions_) {
    if (f1.conditions_.count(kv.first) == 0) {
      collectConditionVars(kv.first, killed_rhs);
    }
  }

  // Canonicalize killed sets.
  std::set<AbstractValue> killed;
  for (AbstractValue v : killed_lhs) {
    killed.insert(f1.findRepReadOnly(v));
    killed.insert(f2.findRepReadOnly(v));
  }
  for (AbstractValue v : killed_rhs) {
    killed.insert(f1.findRepReadOnly(v));
    killed.insert(f2.findRepReadOnly(v));
  }

  // Equalities: keep any pair (u, v) that holds in BOTH f1 and f2, plus
  // equalities involving variables local to one side that are not killed.
  std::unordered_set<AbstractValue> f1_vars;
  std::unordered_set<AbstractValue> f2_vars;
  f1.collectReferencedValues(f1_vars);
  f2.collectReferencedValues(f2_vars);

  constexpr size_t kMaxJoinedEqualities = 256;
  for (const auto &eq : f1.equalities_) {
    if (out.equalities_.size() >= kMaxJoinedEqualities)
      break;
    AbstractValue a = eq.first;
    AbstractValue b = f1.findRepReadOnly(eq.second);
    if (a == b)
      continue;
    if (f2.areEqual(a, b)) {
      (void)out.addEquality(a, b);
    } else if (f2_vars.count(a) == 0 && f2_vars.count(b) == 0 &&
               killed.count(a) == 0 && killed.count(b) == 0) {
      (void)out.addEquality(a, b);
    }
  }
  for (const auto &eq : f2.equalities_) {
    if (out.equalities_.size() >= kMaxJoinedEqualities)
      break;
    AbstractValue a = eq.first;
    AbstractValue b = f2.findRepReadOnly(eq.second);
    if (a == b)
      continue;
    if (f1_vars.count(a) == 0 && f1_vars.count(b) == 0 &&
        killed.count(a) == 0 && killed.count(b) == 0) {
      (void)out.addEquality(a, b);
    }
  }

  // Populate intersected branch conditions AFTER equalities so `addEquality`
  // above does not repeatedly rebuild `out.conditions_`.
  for (const auto &kv : joined_conditions) {
    out.recordCondition(kv.first, kv.second);
  }

  // Null/non-null: intersection (plus non-killed one-sided fresh vars).
  for (AbstractValue v : f1.null_values_) {
    AbstractValue r = f1.findRepReadOnly(v);
    if (f2.isNull(r) ||
        (f2_vars.count(r) == 0 && killed.count(r) == 0)) {
      (void)out.addNull(r);
    }
  }
  for (AbstractValue v : f2.null_values_) {
    AbstractValue r = f2.findRepReadOnly(v);
    if (f1_vars.count(r) == 0 && killed.count(r) == 0) {
      (void)out.addNull(r);
    }
  }
  for (AbstractValue v : f1.non_null_values_) {
    AbstractValue r = f1.findRepReadOnly(v);
    if (f2.isNonNull(r) ||
        (f2_vars.count(r) == 0 && killed.count(r) == 0)) {
      (void)out.addNonNull(r);
    }
  }
  for (AbstractValue v : f2.non_null_values_) {
    AbstractValue r = f2.findRepReadOnly(v);
    if (f1_vars.count(r) == 0 && killed.count(r) == 0) {
      (void)out.addNonNull(r);
    }
  }

  // Disequalities: intersection.
  for (const auto &p : f1.disequalities_) {
    if (f2.areDisequal(p.first, p.second)) {
      (void)out.addDisequality(p.first, p.second);
    }
  }

  // Bounds: join interval [min(lo1, lo2), max(hi1, hi2)] when present on both
  // sides; preserve non-killed one-sided fresh variables.
  for (const auto &kv : f1.lower_bounds_) {
    AbstractValue r = f1.findRepReadOnly(kv.first);
    auto lo2 = f2.getLowerBound(r);
    if (lo2) {
      out.lower_bounds_[out.findRep(r)] = std::min(kv.second, *lo2);
    } else if (f2_vars.count(r) == 0 && killed.count(r) == 0) {
      out.lower_bounds_[out.findRep(r)] = kv.second;
    }
  }
  for (const auto &kv : f2.lower_bounds_) {
    AbstractValue r = f2.findRepReadOnly(kv.first);
    if (f1_vars.count(r) == 0 && killed.count(r) == 0) {
      out.lower_bounds_[out.findRep(r)] = kv.second;
    }
  }
  for (const auto &kv : f1.upper_bounds_) {
    AbstractValue r = f1.findRepReadOnly(kv.first);
    auto hi2 = f2.getUpperBound(r);
    if (hi2) {
      out.upper_bounds_[out.findRep(r)] = std::max(kv.second, *hi2);
    } else if (f2_vars.count(r) == 0 && killed.count(r) == 0) {
      out.upper_bounds_[out.findRep(r)] = kv.second;
    }
  }
  for (const auto &kv : f2.upper_bounds_) {
    AbstractValue r = f2.findRepReadOnly(kv.first);
    if (f1_vars.count(r) == 0 && killed.count(r) == 0) {
      out.upper_bounds_[out.findRep(r)] = kv.second;
    }
  }

  // Integer markers: intersection + fresh vars.
  for (AbstractValue v : f1.integer_values_) {
    AbstractValue r = f1.findRepReadOnly(v);
    if (f2.isInteger(r) || (f2_vars.count(r) == 0 && killed.count(r) == 0)) {
      out.addIntegerConstraint(r);
    }
  }
  for (AbstractValue v : f2.integer_values_) {
    AbstractValue r = f2.findRepReadOnly(v);
    if (f1_vars.count(r) == 0 && killed.count(r) == 0) {
      out.addIntegerConstraint(r);
    }
  }

  // Linear constraints: keep syntactic intersection.
  auto normalize_constraint =
      [](const PulseFormula &f, const LinearConstraint &c) -> LinearConstraint {
    LinearConstraint out_c;
    out_c.constant = c.constant;
    out_c.kind = c.kind;
    out_c.terms = c.terms;
    for (auto &t : out_c.terms) {
      t.var = f.findRepReadOnly(t.var);
    }
    std::sort(out_c.terms.begin(), out_c.terms.end());
    return out_c;
  };

  auto signature = [](const LinearConstraint &c) -> std::string {
    std::string s;
    s.reserve(64);
    s += std::to_string(static_cast<int>(c.kind));
    s += ":";
    s += std::to_string(c.constant);
    for (const auto &t : c.terms) {
      s += "|";
      s += std::to_string(t.var.getId());
      s += ",";
      s += std::to_string(t.coefficient);
    }
    return s;
  };

  if (!f1.linear_constraints_.empty() && !f2.linear_constraints_.empty()) {
    std::multiset<std::string> sigs1;
    for (const auto &c : f1.linear_constraints_) {
      sigs1.insert(signature(normalize_constraint(f1, c)));
    }
    for (const auto &c : f2.linear_constraints_) {
      LinearConstraint nc = normalize_constraint(f2, c);
      std::string sig = signature(nc);
      auto it = sigs1.find(sig);
      if (it != sigs1.end()) {
        sigs1.erase(it);
        if (!out.addLinearConstraint(nc)) {
          return PulseFormula::contradiction();
        }
      }
    }
  }

  // Arithmetic ops: keep syntactic intersection.
  if (!f1.arithmetic_ops_.empty() && !f2.arithmetic_ops_.empty()) {
    auto op_sig = [](const PulseFormula &f, const ArithmeticOp &op) {
      return std::to_string(f.findRepReadOnly(op.result).getId()) + ":" +
             std::to_string(f.findRepReadOnly(op.op1).getId()) + ":" +
             std::to_string(f.findRepReadOnly(op.op2).getId()) + ":" + op.op;
    };
    std::multiset<std::string> op_sigs1;
    for (const auto &op : f1.arithmetic_ops_) {
      op_sigs1.insert(op_sig(f1, op));
    }
    for (const auto &op : f2.arithmetic_ops_) {
      std::string sig = op_sig(f2, op);
      auto it = op_sigs1.find(sig);
      if (it != op_sigs1.end()) {
        op_sigs1.erase(it);
        (void)out.addArithmeticOperation(f2.findRepReadOnly(op.result),
                                         f2.findRepReadOnly(op.op1),
                                         f2.findRepReadOnly(op.op2), op.op);
      }
    }
  }

  // Type / InstanceOf: intersection + non-killed fresh variables.
  for (const auto &kv : f1.exact_types_) {
    AbstractValue r = f1.findRepReadOnly(kv.first);
    auto t2 = f2.getExactType(r);
    if ((t2 && *t2 == kv.second) ||
        (f2_vars.count(r) == 0 && killed.count(r) == 0)) {
      (void)out.addExactType(r, kv.second);
    }
  }
  for (const auto &kv : f2.exact_types_) {
    AbstractValue r = f2.findRepReadOnly(kv.first);
    if (f1_vars.count(r) == 0 && killed.count(r) == 0) {
      (void)out.addExactType(r, kv.second);
    }
  }

  // Float constants: intersection + non-killed fresh variables.
  for (const auto &kv : f1.float_constants_) {
    AbstractValue r = f1.findRepReadOnly(kv.first);
    auto c2 = f2.asConstantFloat(r);
    if ((c2 && *c2 == kv.second) ||
        (f2_vars.count(r) == 0 && killed.count(r) == 0)) {
      (void)out.addFloatConstant(r, kv.second);
    }
  }
  for (const auto &kv : f2.float_constants_) {
    AbstractValue r = f2.findRepReadOnly(kv.first);
    if (f1_vars.count(r) == 0 && killed.count(r) == 0) {
      (void)out.addFloatConstant(r, kv.second);
    }
  }

  for (const auto &kv : f1.bit_widths_) {
    out.setBitWidth(f1.findRepReadOnly(kv.first), kv.second);
  }
  for (const auto &kv : f2.bit_widths_) {
    out.setBitWidth(f2.findRepReadOnly(kv.first), kv.second);
  }

  out.in_tableau_propagate_ = false;
  out.clearNewEqs();
  return out;
}

PulseFormula PulseFormula::clone() const {
  PulseFormula f;
  f.is_contradiction_ = is_contradiction_;
  f.equalities_ = equalities_;
  f.disequalities_ = disequalities_;
  f.non_null_values_ = non_null_values_;
  f.null_values_ = null_values_;
  f.linear_constraints_ = linear_constraints_;
  f.lower_bounds_ = lower_bounds_;
  f.upper_bounds_ = upper_bounds_;
  f.integer_values_ = integer_values_;
  f.arithmetic_ops_ = arithmetic_ops_;
  f.linear_tableau_ = linear_tableau_;
  f.has_linear_inequalities_ = has_linear_inequalities_;
  f.exact_types_ = exact_types_;
  f.instanceof_types_ = instanceof_types_;
  f.not_instanceof_types_ = not_instanceof_types_;
  f.float_constants_ = float_constants_;
  f.float_ops_ = float_ops_;
  f.float_cmps_ = float_cmps_;
  f.bit_widths_ = bit_widths_;
  f.disjunctive_eqs_ = disjunctive_eqs_;
  f.string_constants_ = string_constants_;
  f.string_lengths_ = string_lengths_;
  f.string_concats_ = string_concats_;
  f.string_regex_matches_ = string_regex_matches_;
  f.conditions_ = conditions_;
  f.pending_new_eqs_ = pending_new_eqs_;
  f.sat_cache_mode_ = sat_cache_mode_;
  f.sat_cache_result_ = sat_cache_result_;
  f.consistent_cache_valid_ = consistent_cache_valid_;
  f.consistent_cache_result_ = consistent_cache_result_;
  return f;
}

PulseFormula
PulseFormula::applySubstitution(const Substitution &substitution,
                                unsigned condition_depth_bump) const {
  PulseFormula out;
  if (is_contradiction_) {
    return PulseFormula::contradiction();
  }
  out.in_tableau_propagate_ = true;
  auto sub = [&](AbstractValue v) -> AbstractValue {
    AbstractValue canon = findRepReadOnly(v);
    if (auto mapped = substitution.substitute(canon)) {
      return *mapped;
    }
    return substitution.substituteOrIdentity(v);
  };

  for (const auto &eq : equalities_) {
    AbstractValue lhs = substitution.substituteOrIdentity(eq.first);
    AbstractValue rhs = substitution.substituteOrIdentity(eq.second);
    if (!out.addEquality(lhs, rhs)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &neq : disequalities_) {
    AbstractValue lhs = sub(neq.first);
    AbstractValue rhs = sub(neq.second);
    if (!out.addDisequality(lhs, rhs)) {
      return PulseFormula::contradiction();
    }
  }
  for (AbstractValue v : non_null_values_) {
    if (!out.addNonNull(sub(v))) {
      return PulseFormula::contradiction();
    }
  }
  for (AbstractValue v : null_values_) {
    if (!out.addNull(sub(v))) {
      return PulseFormula::contradiction();
    }
  }

  // Apply substitution to bounds (intersecting if two callee vars map to same
  // caller var).
  for (const auto &kv : lower_bounds_) {
    AbstractValue new_var = sub(kv.first);
    int64_t hi = std::numeric_limits<int64_t>::max();
    auto hi_it = upper_bounds_.find(kv.first);
    if (hi_it != upper_bounds_.end()) {
      hi = hi_it->second;
    }
    if (!out.addBounds(new_var, kv.second, hi)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &kv : upper_bounds_) {
    if (lower_bounds_.count(kv.first) > 0) {
      continue;
    }
    AbstractValue new_var = sub(kv.first);
    if (!out.addBounds(new_var, std::numeric_limits<int64_t>::min(),
                       kv.second)) {
      return PulseFormula::contradiction();
    }
  }

  // Apply substitution to integer markers.
  for (AbstractValue v : integer_values_) {
    out.addIntegerConstraint(sub(v));
  }

  // Apply substitution to linear constraints.
  for (const auto &constraint : linear_constraints_) {
    LinearConstraint new_constraint;
    new_constraint.constant = constraint.constant;
    new_constraint.kind = constraint.kind;
    for (const auto &term : constraint.terms) {
      new_constraint.terms.emplace_back(sub(term.var), term.coefficient);
    }
    if (!out.addLinearConstraint(new_constraint)) {
      return PulseFormula::contradiction();
    }
  }

  // Apply substitution to arithmetic operations (previously dropped!).
  for (const auto &op : arithmetic_ops_) {
    if (!out.addArithmeticOperation(sub(op.result), sub(op.op1), sub(op.op2),
                                    op.op)) {
      return PulseFormula::contradiction();
    }
  }

  // Apply substitution to Type / InstanceOf predicates.
  for (const auto &kv : exact_types_) {
    if (!out.addExactType(sub(kv.first), kv.second)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &kv : instanceof_types_) {
    for (const auto &t : kv.second) {
      if (!out.addInstanceOf(sub(kv.first), t)) {
        return PulseFormula::contradiction();
      }
    }
  }
  for (const auto &kv : not_instanceof_types_) {
    for (const auto &t : kv.second) {
      if (!out.addNotInstanceOf(sub(kv.first), t)) {
        return PulseFormula::contradiction();
      }
    }
  }

  // Apply substitution to Float constants / ops / comparisons / bit-widths /
  // disjunctive equalities.
  for (const auto &kv : float_constants_) {
    if (!out.addFloatConstant(sub(kv.first), kv.second)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &fop : float_ops_) {
    if (!out.addFloatOperation(sub(fop.result), sub(fop.op1), sub(fop.op2),
                               fop.op)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &fcmp : float_cmps_) {
    if (!out.addFloatComparison(sub(fcmp.op1), sub(fcmp.op2), fcmp.pred)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &kv : bit_widths_) {
    out.setBitWidth(sub(kv.first), kv.second);
  }
  for (const auto &deq : disjunctive_eqs_) {
    DisjunctiveEq mapped;
    mapped.target = sub(deq.target);
    mapped.const_options = deq.const_options;
    for (AbstractValue ov : deq.var_options) {
      mapped.var_options.push_back(sub(ov));
    }
    out.disjunctive_eqs_.push_back(std::move(mapped));
  }
  for (const auto &kv : string_constants_) {
    if (!out.addStringConstant(sub(kv.first), kv.second)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &kv : string_lengths_) {
    if (!out.addStringLength(sub(kv.first), sub(kv.second))) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &sc : string_concats_) {
    if (!out.addStringConcat(sub(sc.result), sub(sc.lhs), sub(sc.rhs))) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &rm : string_regex_matches_) {
    if (!out.addStringRegexMatch(sub(rm.value), rm.pattern, rm.positive)) {
      return PulseFormula::contradiction();
    }
  }

  // Apply substitution to path conditions with optional depth bump.
  for (const auto &kv : conditions_) {
    PathConditionAtom mapped = kv.first;
    if (mapped.lhs.getId() != 0) {
      mapped.lhs = sub(mapped.lhs);
    }
    if (!mapped.const_rhs && mapped.rhs.getId() != 0) {
      mapped.rhs = sub(mapped.rhs);
    }
    out.recordCondition(mapped, kv.second + condition_depth_bump);
  }

  out.in_tableau_propagate_ = false;
  if (!out.linear_tableau_.empty()) {
    if (!out.propagateLinearTableau()) {
      return PulseFormula::contradiction();
    }
  }
  if (out.has_linear_inequalities_ && !out.linear_constraints_.empty()) {
    if (!out.propagateLinearInequalities()) {
      return PulseFormula::contradiction();
    }
  }

  return out;
}

std::set<AbstractValue> PulseFormula::simplifyForSummary(
    const std::set<AbstractValue> &keep_vars,
    const std::set<AbstractValue> &precondition_vocab) {
  (void)precondition_vocab;
  std::set<AbstractValue> live_via_arithmetic;
  if (is_contradiction_) {
    return live_via_arithmetic;
  }

  // 1. Prefer a kept variable as representative for each equivalence class.
  std::map<AbstractValue, std::vector<AbstractValue>> classes;
  for (const auto &eq : equalities_) {
    AbstractValue r = findRep(eq.first);
    classes[r].push_back(eq.first);
    classes[r].push_back(eq.second);
  }
  for (AbstractValue kv : keep_vars) {
    AbstractValue r = findRep(kv);
    classes[r].push_back(kv);
  }

  std::map<AbstractValue, AbstractValue> re_rep;
  for (auto &cls_kv : classes) {
    AbstractValue best = cls_kv.first;
    bool best_kept = keep_vars.count(best) > 0;
    for (AbstractValue v : cls_kv.second) {
      bool v_kept = keep_vars.count(v) > 0;
      if ((v_kept && !best_kept) || (v_kept == best_kept && v < best)) {
        best = v;
        best_kept = v_kept;
      }
    }
    for (AbstractValue v : cls_kv.second) {
      re_rep[v] = best;
    }
  }

  auto map_rep = [&](AbstractValue v) -> AbstractValue {
    AbstractValue r = findRepReadOnly(v);
    auto it = re_rep.find(r);
    return (it != re_rep.end()) ? it->second : r;
  };

  // 2. Compute transitive arithmetic reachability from keep_vars so relational
  // constraints linking kept variables (or branch-constraining a kept variable)
  // are preserved, while dead unconstrained local temporaries are dropped.
  std::set<AbstractValue> externally_constrained;
  for (AbstractValue v : null_values_)
    externally_constrained.insert(map_rep(v));
  for (AbstractValue v : non_null_values_)
    externally_constrained.insert(map_rep(v));
  for (const auto &p : disequalities_) {
    externally_constrained.insert(map_rep(p.first));
    externally_constrained.insert(map_rep(p.second));
  }
  for (const auto &c : linear_constraints_) {
    for (const auto &t : c.terms)
      externally_constrained.insert(map_rep(t.var));
  }
  for (const auto &kv : conditions_) {
    std::set<AbstractValue> cvars;
    collectConditionVars(kv.first, cvars);
    for (AbstractValue v : cvars)
      externally_constrained.insert(map_rep(v));
  }
  for (const auto &kv : lower_bounds_)
    externally_constrained.insert(map_rep(kv.first));
  for (const auto &kv : upper_bounds_)
    externally_constrained.insert(map_rep(kv.first));

  std::set<AbstractValue> live;
  for (AbstractValue v : keep_vars) {
    live.insert(map_rep(v));
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &c : linear_constraints_) {
      bool touches_live = false;
      for (const auto &t : c.terms) {
        if (live.count(map_rep(t.var)) > 0) {
          touches_live = true;
          break;
        }
      }
      if (touches_live) {
        for (const auto &t : c.terms) {
          AbstractValue r = map_rep(t.var);
          if (live.insert(r).second) {
            live_via_arithmetic.insert(r);
            changed = true;
          }
        }
      }
    }
    for (const auto &op : arithmetic_ops_) {
      AbstractValue r_res = map_rep(op.result);
      AbstractValue r1 = map_rep(op.op1);
      AbstractValue r2 = map_rep(op.op2);
      const bool res_live = live.count(r_res) > 0;
      const bool inp_live = live.count(r1) > 0 || live.count(r2) > 0;
      const bool res_constrains =
          externally_constrained.count(r_res) > 0 || op.op == "/" ||
          op.op == "%";
      if (res_live || (inp_live && res_constrains)) {
        for (AbstractValue r : {r_res, r1, r2}) {
          if (live.insert(r).second) {
            live_via_arithmetic.insert(r);
            changed = true;
          }
        }
      }
    }
    for (const auto &kv : conditions_) {
      if (kv.second > 0) {
        // Keep variables appearing in depth > 0 conditions so callers can
        // discharge them and decide manifestness.
        std::set<AbstractValue> cvars;
        collectConditionVars(kv.first, cvars);
        for (AbstractValue v : cvars) {
          AbstractValue r = map_rep(v);
          if (live.insert(r).second) {
            live_via_arithmetic.insert(r);
            changed = true;
          }
        }
      }
    }
  }

  // 3. Rebuild compact normalized formula over live variables.
  PulseFormula simplified;
  simplified.in_tableau_propagate_ = true;
  for (const auto &kv : re_rep) {
    if (live.count(kv.first) > 0 && live.count(kv.second) > 0 &&
        !(kv.first == kv.second)) {
      if (!simplified.addEquality(kv.second, kv.first)) {
        is_contradiction_ = true;
        return {};
      }
    }
  }
  for (AbstractValue v : null_values_) {
    AbstractValue r = map_rep(v);
    if (live.count(r) > 0) {
      if (!simplified.addNull(r)) {
        is_contradiction_ = true;
        return {};
      }
    }
  }
  for (AbstractValue v : non_null_values_) {
    AbstractValue r = map_rep(v);
    if (live.count(r) > 0) {
      if (!simplified.addNonNull(r)) {
        is_contradiction_ = true;
        return {};
      }
    }
  }
  for (const auto &p : disequalities_) {
    AbstractValue a = map_rep(p.first);
    AbstractValue b = map_rep(p.second);
    if (live.count(a) > 0 && live.count(b) > 0) {
      if (!simplified.addDisequality(a, b)) {
        is_contradiction_ = true;
        return {};
      }
    }
  }
  for (const auto &kv : lower_bounds_) {
    AbstractValue r = map_rep(kv.first);
    if (live.count(r) > 0) {
      int64_t hi = std::numeric_limits<int64_t>::max();
      auto hi_it = upper_bounds_.find(kv.first);
      if (hi_it != upper_bounds_.end())
        hi = hi_it->second;
      if (!simplified.addBounds(r, kv.second, hi)) {
        is_contradiction_ = true;
        return {};
      }
    }
  }
  for (const auto &kv : upper_bounds_) {
    AbstractValue r = map_rep(kv.first);
    if (live.count(r) > 0 && lower_bounds_.count(kv.first) == 0) {
      if (!simplified.addBounds(r, std::numeric_limits<int64_t>::min(),
                                kv.second)) {
        is_contradiction_ = true;
        return {};
      }
    }
  }
  for (AbstractValue v : integer_values_) {
    AbstractValue r = map_rep(v);
    if (live.count(r) > 0) {
      simplified.addIntegerConstraint(r);
    }
  }
  for (const auto &c : linear_constraints_) {
    bool all_live = !c.terms.empty();
    LinearConstraint nc;
    nc.constant = c.constant;
    nc.kind = c.kind;
    for (const auto &t : c.terms) {
      AbstractValue r = map_rep(t.var);
      if (live.count(r) == 0) {
        all_live = false;
        break;
      }
      nc.terms.emplace_back(r, t.coefficient);
    }
    if (all_live) {
      if (!simplified.addLinearConstraint(nc)) {
        is_contradiction_ = true;
        return {};
      }
    }
  }
  for (const auto &op : arithmetic_ops_) {
    AbstractValue r_res = map_rep(op.result);
    AbstractValue r1 = map_rep(op.op1);
    AbstractValue r2 = map_rep(op.op2);
    if (live.count(r_res) > 0 && live.count(r1) > 0 && live.count(r2) > 0) {
      if (!simplified.addArithmeticOperation(r_res, r1, r2, op.op)) {
        is_contradiction_ = true;
        return {};
      }
    }
  }
  for (const auto &kv : exact_types_) {
    AbstractValue r = map_rep(kv.first);
    if (live.count(r) > 0) {
      if (!simplified.addExactType(r, kv.second)) {
        is_contradiction_ = true;
        return {};
      }
    }
  }
  for (const auto &kv : instanceof_types_) {
    AbstractValue r = map_rep(kv.first);
    if (live.count(r) > 0) {
      for (const auto &t : kv.second) {
        if (!simplified.addInstanceOf(r, t)) {
          is_contradiction_ = true;
          return {};
        }
      }
    }
  }
  for (const auto &kv : not_instanceof_types_) {
    AbstractValue r = map_rep(kv.first);
    if (live.count(r) > 0) {
      for (const auto &t : kv.second) {
        if (!simplified.addNotInstanceOf(r, t)) {
          is_contradiction_ = true;
          return {};
        }
      }
    }
  }
  for (const auto &kv : float_constants_) {
    AbstractValue r = map_rep(kv.first);
    if (live.count(r) > 0) {
      (void)simplified.addFloatConstant(r, kv.second);
    }
  }
  for (const auto &fop : float_ops_) {
    AbstractValue r_res = map_rep(fop.result);
    AbstractValue r1 = map_rep(fop.op1);
    AbstractValue r2 = map_rep(fop.op2);
    if (live.count(r_res) > 0 && live.count(r1) > 0 && live.count(r2) > 0) {
      (void)simplified.addFloatOperation(r_res, r1, r2, fop.op);
    }
  }
  for (const auto &fcmp : float_cmps_) {
    AbstractValue r1 = map_rep(fcmp.op1);
    AbstractValue r2 = map_rep(fcmp.op2);
    if (live.count(r1) > 0 && live.count(r2) > 0) {
      (void)simplified.addFloatComparison(r1, r2, fcmp.pred);
    }
  }
  for (const auto &kv : bit_widths_) {
    AbstractValue r = map_rep(kv.first);
    if (live.count(r) > 0) {
      simplified.setBitWidth(r, kv.second);
    }
  }
  for (const auto &deq : disjunctive_eqs_) {
    AbstractValue rt = map_rep(deq.target);
    if (live.count(rt) == 0)
      continue;
    DisjunctiveEq mapped;
    mapped.target = rt;
    mapped.const_options = deq.const_options;
    bool all_vars_live = true;
    for (AbstractValue ov : deq.var_options) {
      AbstractValue ro = map_rep(ov);
      if (live.count(ro) == 0) {
        all_vars_live = false;
        break;
      }
      mapped.var_options.push_back(ro);
    }
    if (all_vars_live &&
        (!mapped.const_options.empty() || !mapped.var_options.empty())) {
      simplified.disjunctive_eqs_.push_back(std::move(mapped));
    }
  }
  for (const auto &kv : string_constants_) {
    AbstractValue r = map_rep(kv.first);
    if (live.count(r) > 0) {
      (void)simplified.addStringConstant(r, kv.second);
    }
  }
  for (const auto &kv : string_lengths_) {
    AbstractValue rs = map_rep(kv.first);
    AbstractValue rl = map_rep(kv.second);
    if (live.count(rs) > 0 && live.count(rl) > 0) {
      (void)simplified.addStringLength(rs, rl);
    }
  }
  for (const auto &sc : string_concats_) {
    AbstractValue rr = map_rep(sc.result);
    AbstractValue r1 = map_rep(sc.lhs);
    AbstractValue r2 = map_rep(sc.rhs);
    if (live.count(rr) > 0 && live.count(r1) > 0 && live.count(r2) > 0) {
      (void)simplified.addStringConcat(rr, r1, r2);
    }
  }
  for (const auto &rm : string_regex_matches_) {
    AbstractValue r = map_rep(rm.value);
    if (live.count(r) > 0) {
      (void)simplified.addStringRegexMatch(r, rm.pattern, rm.positive);
    }
  }
  for (const auto &kv : conditions_) {
    PathConditionAtom mapped = kv.first;
    if (mapped.lhs.getId() != 0)
      mapped.lhs = map_rep(mapped.lhs);
    if (!mapped.const_rhs && mapped.rhs.getId() != 0)
      mapped.rhs = map_rep(mapped.rhs);
    bool keep_cond = kv.second > 0;
    if (!keep_cond) {
      if (mapped.lhs.getId() != 0 && live.count(mapped.lhs) > 0 &&
          (mapped.const_rhs || mapped.rhs.getId() == 0 ||
           live.count(mapped.rhs) > 0)) {
        keep_cond = true;
      }
    }
    if (keep_cond) {
      simplified.recordCondition(mapped, kv.second);
    }
  }

  simplified.in_tableau_propagate_ = false;
  *this = std::move(simplified);
  return live_via_arithmetic;
}

std::set<AbstractValue> PulseFormula::getReferencedValues() const {
  std::set<AbstractValue> vars;
  for (const auto &eq : equalities_) {
    vars.insert(eq.first);
    vars.insert(eq.second);
  }
  for (const auto &p : disequalities_) {
    vars.insert(p.first);
    vars.insert(p.second);
  }
  for (AbstractValue v : null_values_)
    vars.insert(v);
  for (AbstractValue v : non_null_values_)
    vars.insert(v);
  for (const auto &kv : lower_bounds_)
    vars.insert(kv.first);
  for (const auto &kv : upper_bounds_)
    vars.insert(kv.first);
  for (AbstractValue v : integer_values_)
    vars.insert(v);
  for (const auto &c : linear_constraints_) {
    for (const auto &t : c.terms)
      vars.insert(t.var);
  }
  for (const auto &op : arithmetic_ops_) {
    vars.insert(op.result);
    vars.insert(op.op1);
    vars.insert(op.op2);
  }
  for (const auto &kv : exact_types_)
    vars.insert(kv.first);
  for (const auto &kv : instanceof_types_)
    vars.insert(kv.first);
  for (const auto &kv : not_instanceof_types_)
    vars.insert(kv.first);
  for (const auto &kv : float_constants_)
    vars.insert(kv.first);
  for (const auto &fop : float_ops_) {
    vars.insert(fop.result);
    vars.insert(fop.op1);
    vars.insert(fop.op2);
  }
  for (const auto &fcmp : float_cmps_) {
    vars.insert(fcmp.op1);
    vars.insert(fcmp.op2);
  }
  for (const auto &deq : disjunctive_eqs_) {
    vars.insert(deq.target);
    for (AbstractValue ov : deq.var_options)
      vars.insert(ov);
  }
  for (const auto &kv : string_constants_)
    vars.insert(kv.first);
  for (const auto &kv : string_lengths_) {
    vars.insert(kv.first);
    vars.insert(kv.second);
  }
  for (const auto &sc : string_concats_) {
    vars.insert(sc.result);
    vars.insert(sc.lhs);
    vars.insert(sc.rhs);
  }
  for (const auto &rm : string_regex_matches_)
    vars.insert(rm.value);
  for (const auto &kv : conditions_) {
    collectConditionVars(kv.first, vars);
  }
  return vars;
}

void PulseFormula::collectReferencedValues(
    std::unordered_set<AbstractValue> &vars) const {
  const size_t approx =
      equalities_.size() * 2 + disequalities_.size() * 2 + null_values_.size() +
      non_null_values_.size() + lower_bounds_.size() + upper_bounds_.size() +
      integer_values_.size() + linear_constraints_.size() * 2 +
      arithmetic_ops_.size() * 3 + 16;
  vars.reserve(vars.size() + approx);
  for (const auto &eq : equalities_) {
    vars.insert(eq.first);
    vars.insert(eq.second);
  }
  for (const auto &p : disequalities_) {
    vars.insert(p.first);
    vars.insert(p.second);
  }
  for (AbstractValue v : null_values_)
    vars.insert(v);
  for (AbstractValue v : non_null_values_)
    vars.insert(v);
  for (const auto &kv : lower_bounds_)
    vars.insert(kv.first);
  for (const auto &kv : upper_bounds_)
    vars.insert(kv.first);
  for (AbstractValue v : integer_values_)
    vars.insert(v);
  for (const auto &c : linear_constraints_) {
    for (const auto &t : c.terms)
      vars.insert(t.var);
  }
  for (const auto &op : arithmetic_ops_) {
    vars.insert(op.result);
    vars.insert(op.op1);
    vars.insert(op.op2);
  }
  for (const auto &kv : exact_types_)
    vars.insert(kv.first);
  for (const auto &kv : instanceof_types_)
    vars.insert(kv.first);
  for (const auto &kv : not_instanceof_types_)
    vars.insert(kv.first);
  for (const auto &kv : float_constants_)
    vars.insert(kv.first);
  for (const auto &fop : float_ops_) {
    vars.insert(fop.result);
    vars.insert(fop.op1);
    vars.insert(fop.op2);
  }
  for (const auto &fcmp : float_cmps_) {
    vars.insert(fcmp.op1);
    vars.insert(fcmp.op2);
  }
  for (const auto &deq : disjunctive_eqs_) {
    vars.insert(deq.target);
    for (AbstractValue ov : deq.var_options)
      vars.insert(ov);
  }
  for (const auto &kv : string_constants_)
    vars.insert(kv.first);
  for (const auto &kv : string_lengths_) {
    vars.insert(kv.first);
    vars.insert(kv.second);
  }
  for (const auto &sc : string_concats_) {
    vars.insert(sc.result);
    vars.insert(sc.lhs);
    vars.insert(sc.rhs);
  }
  for (const auto &rm : string_regex_matches_)
    vars.insert(rm.value);
  for (const auto &kv : conditions_) {
    if (kv.first.lhs.getId() != 0)
      vars.insert(kv.first.lhs);
    if (!kv.first.const_rhs && kv.first.rhs.getId() != 0)
      vars.insert(kv.first.rhs);
  }
}

bool PulseFormula::hasValueConstraints(AbstractValue v) const {
  AbstractValue rep = findRepReadOnly(v);
  if (null_values_.count(rep) > 0 || non_null_values_.count(rep) > 0 ||
      lower_bounds_.count(rep) > 0 || upper_bounds_.count(rep) > 0 ||
      exact_types_.count(rep) > 0) {
    return true;
  }
  for (const auto &lc : linear_constraints_) {
    for (const auto &t : lc.terms) {
      if (findRepReadOnly(t.var) == rep)
        return true;
    }
  }
  for (const auto &op : arithmetic_ops_) {
    if (findRepReadOnly(op.result) == rep || findRepReadOnly(op.op1) == rep ||
        findRepReadOnly(op.op2) == rep) {
      return true;
    }
  }
  return false;
}

bool PulseFormula::equivalentTo(const PulseFormula &other) const {
  auto normalize_value_set = [](const PulseFormula &formula,
                                const std::set<AbstractValue> &values) {
    std::set<AbstractValue> normalized;
    for (AbstractValue v : values) {
      normalized.insert(formula.findRepReadOnly(v));
    }
    return normalized;
  };

  auto normalize_pairs = [](const PulseFormula &formula,
                            const std::set<std::pair<AbstractValue,
                                                     AbstractValue>> &pairs) {
    std::set<std::pair<AbstractValue, AbstractValue>> normalized;
    for (const auto &p : pairs) {
      normalized.insert(normalizePair(formula.findRepReadOnly(p.first),
                                      formula.findRepReadOnly(p.second)));
    }
    return normalized;
  };

  auto normalize_bounds =
      [](const PulseFormula &formula,
         const std::map<AbstractValue, int64_t> &bounds) {
        std::map<AbstractValue, int64_t> normalized;
        for (const auto &kv : bounds) {
          normalized[formula.findRepReadOnly(kv.first)] = kv.second;
        }
        return normalized;
      };

  auto normalize_linear_constraints = [](const PulseFormula &formula) {
    std::multiset<std::string> normalized;
    for (const auto &constraint : formula.linear_constraints_) {
      std::vector<LinearTerm> terms = constraint.terms;
      for (auto &term : terms) {
        term.var = formula.findRepReadOnly(term.var);
      }
      std::sort(terms.begin(), terms.end());

      std::string sig;
      sig.reserve(64);
      sig += std::to_string(static_cast<int>(constraint.kind));
      sig += ":";
      sig += std::to_string(constraint.constant);
      for (const auto &term : terms) {
        sig += "|";
        sig += std::to_string(term.var.getId());
        sig += ",";
        sig += std::to_string(term.coefficient);
      }
      normalized.insert(std::move(sig));
    }
    return normalized;
  };

  auto normalize_arithmetic_ops = [](const PulseFormula &formula) {
    std::multiset<std::string> normalized;
    for (const auto &op : formula.arithmetic_ops_) {
      std::string sig;
      sig.reserve(48);
      sig += std::to_string(formula.findRepReadOnly(op.result).getId());
      sig += ":";
      sig += std::to_string(formula.findRepReadOnly(op.op1).getId());
      sig += ":";
      sig += std::to_string(formula.findRepReadOnly(op.op2).getId());
      sig += ":";
      sig += op.op;
      normalized.insert(std::move(sig));
    }
    return normalized;
  };

  auto normalize_equalities = [](const PulseFormula &formula) {
    std::set<std::pair<AbstractValue, AbstractValue>> normalized;
    std::set<AbstractValue> values;
    for (const auto &eq : formula.equalities_) {
      values.insert(eq.first);
      values.insert(eq.second);
    }
    for (AbstractValue value : values) {
      AbstractValue rep = formula.findRepReadOnly(value);
      if (!(value == rep)) {
        normalized.insert(normalizePair(value, rep));
      }
    }
    return normalized;
  };

  return is_contradiction_ == other.is_contradiction_ &&
         normalize_equalities(*this) == normalize_equalities(other) &&
         normalize_pairs(*this, disequalities_) ==
             normalize_pairs(other, other.disequalities_) &&
         normalize_value_set(*this, null_values_) ==
             normalize_value_set(other, other.null_values_) &&
         normalize_value_set(*this, non_null_values_) ==
             normalize_value_set(other, other.non_null_values_) &&
         normalize_bounds(*this, lower_bounds_) ==
             normalize_bounds(other, other.lower_bounds_) &&
         normalize_bounds(*this, upper_bounds_) ==
             normalize_bounds(other, other.upper_bounds_) &&
         normalize_value_set(*this, integer_values_) ==
             normalize_value_set(other, other.integer_values_) &&
         normalize_linear_constraints(*this) ==
             normalize_linear_constraints(other) &&
         normalize_arithmetic_ops(*this) == normalize_arithmetic_ops(other);
}

void PulseFormula::addIntegerConstraint(AbstractValue v) {
  if (is_contradiction_) {
    return;
  }
  AbstractValue rep = findRep(v);
  integer_values_.insert(rep);
}

bool PulseFormula::addLinearConstraint(const LinearConstraint &constraint) {
  if (is_contradiction_) {
    return false;
  }

  int64_t min_value = 0, max_value = 0;
  bool has_bounds = false;

  for (const auto &term : constraint.terms) {
    AbstractValue rep = findRep(term.var);
    auto lower_it = lower_bounds_.find(rep);
    auto upper_it = upper_bounds_.find(rep);

    int64_t term_min = (lower_it != lower_bounds_.end())
                           ? lower_it->second
                           : std::numeric_limits<int64_t>::min();
    int64_t term_max = (upper_it != upper_bounds_.end())
                           ? upper_it->second
                           : std::numeric_limits<int64_t>::max();

    if (term.coefficient > 0) {
      min_value = satAddI64(min_value, satMulI64(term.coefficient, term_min));
      max_value = satAddI64(max_value, satMulI64(term.coefficient, term_max));
    } else {
      min_value = satAddI64(min_value, satMulI64(term.coefficient, term_max));
      max_value = satAddI64(max_value, satMulI64(term.coefficient, term_min));
    }
    has_bounds = true;
  }

  if (has_bounds) {
    int64_t rhs = constraint.constant;
    switch (constraint.kind) {
    case ConstraintKind::LessEqual:
      if (min_value > rhs)
        return false;
      break;
    case ConstraintKind::Less:
      if (min_value >= rhs)
        return false;
      break;
    case ConstraintKind::GreaterEqual:
      if (max_value < rhs)
        return false;
      break;
    case ConstraintKind::Greater:
      if (max_value <= rhs)
        return false;
      break;
    case ConstraintKind::Equal:
      if (min_value > rhs || max_value < rhs)
        return false;
      break;
    }
  }

  // Deduplicate canonical linear constraints and cap total count to keep
  // interprocedural summary merging linear.
  LinearConstraint canonical_lc;
  canonical_lc.constant = constraint.constant;
  canonical_lc.kind = constraint.kind;
  canonical_lc.terms.reserve(constraint.terms.size());
  for (const auto &term : constraint.terms) {
    if (term.coefficient != 0) {
      canonical_lc.terms.emplace_back(findRep(term.var), term.coefficient);
    }
  }
  for (const auto &existing : linear_constraints_) {
    if (existing.kind == canonical_lc.kind &&
        existing.constant == canonical_lc.constant &&
        existing.terms.size() == canonical_lc.terms.size()) {
      bool same = true;
      for (size_t i = 0; i < existing.terms.size(); ++i) {
        if (!(findRep(existing.terms[i].var) == canonical_lc.terms[i].var) ||
            existing.terms[i].coefficient != canonical_lc.terms[i].coefficient) {
          same = false;
          break;
        }
      }
      if (same) {
        return true;
      }
    }
  }

  constexpr size_t kMaxLinearConstraints = 128;
  if (linear_constraints_.size() >= kMaxLinearConstraints) {
    return in_tableau_propagate_ ? true : isConsistent();
  }

  invalidateSatOnly();
  if (canonical_lc.kind != ConstraintKind::Equal) {
    has_linear_inequalities_ = true;
  }
  linear_constraints_.push_back(canonical_lc);
  if (canonical_lc.kind == ConstraintKind::Equal) {
    LinearEqRow row;
    row.constant = -canonical_lc.constant;
    for (const auto &term : canonical_lc.terms) {
      row.coeffs[term.var] = satAddI64(row.coeffs[term.var], term.coefficient);
      if (row.coeffs[term.var] == 0) {
        row.coeffs.erase(term.var);
      }
    }
    if (!insertLinearEquationRow(std::move(row))) {
      return false;
    }
    if (!in_tableau_propagate_ && !propagateLinearTableau()) {
      return false;
    }
    // Equalities from +/- SSA definitions are already checked by the RREF
    // tableau; defer full Z3 solving to branch assumptions / isUnsat().
    return in_tableau_propagate_ ? true : isConsistent();
  } else {
    if (!in_tableau_propagate_ && !propagateLinearInequalities()) {
      return false;
    }
  }
  return in_tableau_propagate_ ? true : isConsistent();
}

bool PulseFormula::addBounds(AbstractValue v, int64_t lower, int64_t upper) {
  if (is_contradiction_) {
    return false;
  }
  if (lower > upper) {
    return false;
  }

  AbstractValue rep = findRep(v);
  int64_t new_lower = lower;
  int64_t new_upper = upper;

  auto lower_it = lower_bounds_.find(rep);
  if (lower_it != lower_bounds_.end()) {
    new_lower = std::max(lower_it->second, lower);
  }
  auto upper_it = upper_bounds_.find(rep);
  if (upper_it != upper_bounds_.end()) {
    new_upper = std::min(upper_it->second, upper);
  }

  if (new_lower > new_upper) {
    return false;
  }

  // Cross-check with null / non-null constraints.
  if (null_values_.count(rep) > 0 && (new_lower > 0 || new_upper < 0)) {
    return false;
  }
  if (non_null_values_.count(rep) > 0 && new_lower == 0 && new_upper == 0) {
    return false;
  }

  const bool bounds_changed =
      lower_it == lower_bounds_.end() || lower_it->second != new_lower ||
      upper_it == upper_bounds_.end() || upper_it->second != new_upper;
  if (!bounds_changed) {
    return true;
  }
  invalidateSatCache();
  lower_bounds_[rep] = new_lower;
  upper_bounds_[rep] = new_upper;

  if (new_lower == 0 && new_upper == 0) {
    if (null_values_.insert(rep).second) {
      pending_new_eqs_.push_back(NewEq::makeEqZero(rep));
    }
  } else if (new_lower > 0 || new_upper < 0) {
    non_null_values_.insert(rep);
  }

  if (!in_tableau_propagate_ && !linear_tableau_.empty()) {
    if (!propagateLinearTableau()) {
      return false;
    }
  }
  if (!in_tableau_propagate_ && has_linear_inequalities_ &&
      !linear_constraints_.empty()) {
    if (!propagateLinearInequalities()) {
      return false;
    }
  }

  return true;
}

bool PulseFormula::isInteger(AbstractValue v) const {
  AbstractValue rep = findRepReadOnly(v);
  return integer_values_.count(rep) > 0;
}

std::optional<int64_t> PulseFormula::getLowerBound(AbstractValue v) const {
  AbstractValue rep = findRepReadOnly(v);
  auto it = lower_bounds_.find(rep);
  if (it != lower_bounds_.end()) {
    return it->second;
  }
  if (null_values_.count(rep) > 0) {
    return 0;
  }
  return std::nullopt;
}

std::optional<int64_t> PulseFormula::getUpperBound(AbstractValue v) const {
  AbstractValue rep = findRepReadOnly(v);
  auto it = upper_bounds_.find(rep);
  if (it != upper_bounds_.end()) {
    return it->second;
  }
  if (null_values_.count(rep) > 0) {
    return 0;
  }
  return std::nullopt;
}

std::optional<int64_t> PulseFormula::asConstantInt(AbstractValue v) const {
  if (v.getId() == 0)
    return std::nullopt;
  auto lo = getLowerBound(v);
  auto hi = getUpperBound(v);
  if (lo && hi && *lo == *hi) {
    return *lo;
  }
  return std::nullopt;
}

bool PulseFormula::isUnsat() const {
  if (is_contradiction_) {
    return true;
  }
  return !checkSatisfiability();
}

bool PulseFormula::checkSatisfiability() const {
  if (is_contradiction_) {
    return false;
  }
  const uint8_t cur_mode = pulse::options::disableSMT() ? 1u : 2u;
  if (sat_cache_mode_ == cur_mode ||
      (sat_cache_mode_ == 2u && cur_mode == 1u && sat_cache_result_)) {
    return sat_cache_result_;
  }
  auto cacheAndReturn = [&](bool res) {
    sat_cache_mode_ = cur_mode;
    sat_cache_result_ = res;
    return res;
  };
  if (!isConsistent()) {
    return cacheAndReturn(false);
  }
  // Pure equalities, disequalities, null/non-null, unary interval bounds,
  // types, and constant strings are completely decided by isConsistent();
  // skip Z3 unless relational/arithmetic/FP/disjunctive constraints exist.
  if (linear_constraints_.empty() && arithmetic_ops_.empty() &&
      float_ops_.empty() && float_cmps_.empty() && disjunctive_eqs_.empty()) {
    return cacheAndReturn(true);
  }

  if (cur_mode == 1u) {
    return cacheAndReturn(true);
  }

  // Slice arithmetic_ops_ down to definitions reachable from constrained
  // variables: an unconstrained fresh SSA definition `r = a op b` is always
  // satisfiable and must not trigger Z3 (especially QF_BV bit-blasting).
  std::vector<ArithmeticOp> active_arith_ops;
  if (!arithmetic_ops_.empty()) {
    std::set<AbstractValue> external_constrained;
    for (AbstractValue v : null_values_)
      external_constrained.insert(findRepReadOnly(v));
    for (const auto &neq : disequalities_) {
      external_constrained.insert(findRepReadOnly(neq.first));
      external_constrained.insert(findRepReadOnly(neq.second));
    }
    for (const auto &lc : linear_constraints_) {
      for (const auto &t : lc.terms)
        external_constrained.insert(findRepReadOnly(t.var));
    }
    std::set<AbstractValue> constrained = external_constrained;
    for (const auto &kv : lower_bounds_)
      constrained.insert(findRepReadOnly(kv.first));
    for (const auto &kv : upper_bounds_)
      constrained.insert(findRepReadOnly(kv.first));
    for (AbstractValue v : non_null_values_)
      constrained.insert(findRepReadOnly(v));

    // Only activate a disjunctive equality `t = o1 || o2` if `t` itself is
    // constrained (otherwise `t` is an unconstrained fresh join value that
    // cannot constrain `o1` or `o2` or trigger QF_BV).
    if (!disjunctive_eqs_.empty()) {
      std::map<AbstractValue, unsigned> disj_target_uses;
      for (const auto &deq : disjunctive_eqs_) {
        disj_target_uses[findRepReadOnly(deq.target)]++;
        for (AbstractValue ov : deq.var_options)
          disj_target_uses[findRepReadOnly(ov)]++;
      }
      for (const auto &op : arithmetic_ops_) {
        disj_target_uses[findRepReadOnly(op.op1)]++;
        disj_target_uses[findRepReadOnly(op.op2)]++;
      }
      for (const auto &deq : disjunctive_eqs_) {
        AbstractValue t = findRepReadOnly(deq.target);
        if (constrained.count(t) > 0 || disj_target_uses[t] > 1) {
          external_constrained.insert(t);
          constrained.insert(t);
          for (AbstractValue ov : deq.var_options) {
            AbstractValue ro = findRepReadOnly(ov);
            external_constrained.insert(ro);
            constrained.insert(ro);
          }
        }
      }
    }

    std::map<AbstractValue, unsigned> result_def_count;
    for (const auto &op : arithmetic_ops_) {
      result_def_count[findRepReadOnly(op.result)]++;
    }
    auto isOnlyIntrinsicBitwiseBound = [&](const ArithmeticOp &op,
                                           AbstractValue r, AbstractValue o1,
                                           AbstractValue o2) -> bool {
      if (external_constrained.count(r) > 0 || result_def_count[r] > 1 ||
          r == o1 || r == o2) {
        return false;
      }
      if (op.op == "&") {
        if (non_null_values_.count(r) > 0)
          return false;
        auto lo = getLowerBound(r);
        auto hi = getUpperBound(r);
        auto c1 = asConstantInt(o1);
        auto c2 = asConstantInt(o2);
        if (lo && hi && *lo == 0 &&
            ((c1 && *c1 >= 0 && *hi == *c1) || (c2 && *c2 >= 0 && *hi == *c2))) {
          return true;
        }
      } else if (op.op == "|") {
        if (getLowerBound(r).has_value() || getUpperBound(r).has_value())
          return false;
        auto c1 = asConstantInt(o1);
        auto c2 = asConstantInt(o2);
        if ((c1 && *c1 != 0) || (c2 && *c2 != 0)) {
          return true;
        }
      }
      return false;
    };

    std::vector<bool> keep(arithmetic_ops_.size(), false);
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t i = 0; i < arithmetic_ops_.size(); ++i) {
        if (keep[i])
          continue;
        const auto &op = arithmetic_ops_[i];
        AbstractValue r = findRepReadOnly(op.result);
        AbstractValue o1 = findRepReadOnly(op.op1);
        AbstractValue o2 = findRepReadOnly(op.op2);
        if ((op.op == "/" || op.op == "%") && isNull(o2)) {
          return cacheAndReturn(false);
        }
        const bool div_needs_nonzero =
            (op.op == "/" || op.op == "%") && !isNonNull(o2) &&
            (external_constrained.count(o2) > 0 || result_def_count[o2] > 0);
        const bool res_constrained =
            (constrained.count(r) > 0 && !isOnlyIntrinsicBitwiseBound(op, r, o1, o2)) ||
            result_def_count[r] > 1 || r == o1 || r == o2;
        if (div_needs_nonzero || res_constrained) {
          keep[i] = true;
          external_constrained.insert(r);
          external_constrained.insert(o1);
          external_constrained.insert(o2);
          constrained.insert(r);
          constrained.insert(o1);
          constrained.insert(o2);
          changed = true;
        }
      }
    }
    for (size_t i = 0; i < arithmetic_ops_.size(); ++i) {
      if (keep[i]) {
        active_arith_ops.push_back(arithmetic_ops_[i]);
      }
    }
  }

  // Check whether any linear constraint actually requires Z3 beyond our exact
  // RREF equality tableau + Floyd-Warshall difference-constraint solver.
  bool needs_linear_smt = false;
  if (!linear_constraints_.empty()) {
    constexpr size_t kMaxTableauRows = 48;
    const bool tableau_complete = linear_tableau_.size() < kMaxTableauRows;
    if (!has_linear_inequalities_) {
      // Pure linear equalities are completely decided by RREF linear_tableau_
      // unless the tableau hit its row cap.
      needs_linear_smt = !tableau_complete;
    } else {
      // Check if any inequality is non-difference (not `u - v <= c`) or
      // interacts with tableau equalities, or if difference graph > 32 nodes.
      std::set<AbstractValue> diff_nodes;
      for (const auto &lc : linear_constraints_) {
        if (lc.kind == ConstraintKind::Equal)
          continue;
        bool is_unit_diff =
            lc.terms.size() == 2 &&
            ((lc.terms[0].coefficient == 1 && lc.terms[1].coefficient == -1) ||
             (lc.terms[0].coefficient == -1 && lc.terms[1].coefficient == 1));
        if (!is_unit_diff) {
          needs_linear_smt = true;
          break;
        }
        AbstractValue v0 = findRepReadOnly(lc.terms[0].var);
        AbstractValue v1 = findRepReadOnly(lc.terms[1].var);
        diff_nodes.insert(v0);
        diff_nodes.insert(v1);
        if (!linear_tableau_.empty()) {
          // If any variable in the inequality also appears in a tableau row,
          // let Z3 decide the combined LIA system.
          for (const auto &trow : linear_tableau_) {
            if (trow.second.coeffs.count(v0) > 0 ||
                trow.second.coeffs.count(v1) > 0) {
              needs_linear_smt = true;
              break;
            }
          }
          if (needs_linear_smt)
            break;
        }
      }
      if (diff_nodes.size() > 32) {
        needs_linear_smt = true;
      }
    }
  }

  // Filter disjunctive equalities: fast-check unary satisfaction/contradiction
  // for BOTH constant and variable disjunctions, dropping independent
  // disjunctions that already have a unary-compatible assignment.
  std::vector<DisjunctiveEq> active_disj_eqs;
  if (!disjunctive_eqs_.empty()) {
    std::set<AbstractValue> rel_touched;
    std::map<AbstractValue, unsigned> disj_var_count;
    for (const auto &lc : linear_constraints_) {
      for (const auto &t : lc.terms)
        rel_touched.insert(findRepReadOnly(t.var));
    }
    for (const auto &op : active_arith_ops) {
      rel_touched.insert(findRepReadOnly(op.result));
      rel_touched.insert(findRepReadOnly(op.op1));
      rel_touched.insert(findRepReadOnly(op.op2));
    }
    for (const auto &neq : disequalities_) {
      rel_touched.insert(findRepReadOnly(neq.first));
      rel_touched.insert(findRepReadOnly(neq.second));
    }
    for (const auto &deq : disjunctive_eqs_) {
      disj_var_count[findRepReadOnly(deq.target)]++;
      for (AbstractValue ov : deq.var_options) {
        disj_var_count[findRepReadOnly(ov)]++;
      }
    }

    auto varCompatible = [&](AbstractValue a, AbstractValue b) -> bool {
      if (a == b)
        return true;
      if (areDisequal(a, b))
        return false;
      const bool a_null = null_values_.count(a) > 0;
      const bool b_null = null_values_.count(b) > 0;
      const bool a_nn = non_null_values_.count(a) > 0;
      const bool b_nn = non_null_values_.count(b) > 0;
      if ((a_null && b_nn) || (b_null && a_nn))
        return false;
      auto a_lo = getLowerBound(a);
      auto a_hi = getUpperBound(a);
      auto b_lo = getLowerBound(b);
      auto b_hi = getUpperBound(b);
      std::optional<int64_t> lo =
          (a_lo && b_lo) ? std::optional<int64_t>(std::max(*a_lo, *b_lo))
                         : (a_lo ? a_lo : b_lo);
      std::optional<int64_t> hi =
          (a_hi && b_hi) ? std::optional<int64_t>(std::min(*a_hi, *b_hi))
                         : (a_hi ? a_hi : b_hi);
      if (lo && hi && *lo > *hi)
        return false;
      if ((a_null || b_null) && ((lo && *lo > 0) || (hi && *hi < 0)))
        return false;
      if ((a_nn || b_nn) && lo && hi && *lo == 0 && *hi == 0)
        return false;
      return true;
    };

    for (const auto &deq : disjunctive_eqs_) {
      AbstractValue t = findRepReadOnly(deq.target);
      auto lo = getLowerBound(t);
      auto hi = getUpperBound(t);
      const bool must_null = null_values_.count(t) > 0;
      const bool must_nonnull = non_null_values_.count(t) > 0;

      bool any_const_ok = false;
      for (int64_t c : deq.const_options) {
        if (lo && c < *lo)
          continue;
        if (hi && c > *hi)
          continue;
        if (must_null && c != 0)
          continue;
        if (must_nonnull && c == 0)
          continue;
        any_const_ok = true;
        break;
      }

      bool any_var_ok = false;
      bool any_indep_var_ok = false;
      for (AbstractValue ov_raw : deq.var_options) {
        AbstractValue ov = findRepReadOnly(ov_raw);
        if (varCompatible(t, ov)) {
          any_var_ok = true;
          if (rel_touched.count(ov) == 0 && disj_var_count[ov] <= 1) {
            any_indep_var_ok = true;
          }
        }
      }

      if (!any_const_ok && !any_var_ok &&
          (!deq.const_options.empty() || !deq.var_options.empty())) {
        return cacheAndReturn(false);
      }
      if (rel_touched.count(t) == 0 && disj_var_count[t] <= 1 &&
          (any_const_ok || any_indep_var_ok)) {
        continue; // Satisfied independently by a unary-compatible option.
      }
      if (active_disj_eqs.size() < 8) {
        active_disj_eqs.push_back(deq);
      }
    }
  }

  // Peel off single-use arithmetic/bitwise ops whose inputs are completely free
  // and whose result's unary constraint is trivially satisfiable (e.g., C flag
  // checks `if (flags & MASK)` where `flags` has no other constraints).
  if (!active_arith_ops.empty()) {
    std::map<AbstractValue, unsigned> ext_use;
    std::map<AbstractValue, unsigned> arith_use;
    for (const auto &lc : linear_constraints_) {
      for (const auto &t : lc.terms)
        ext_use[findRepReadOnly(t.var)]++;
    }
    for (const auto &deq : active_disj_eqs) {
      ext_use[findRepReadOnly(deq.target)]++;
      for (AbstractValue ov : deq.var_options)
        ext_use[findRepReadOnly(ov)]++;
    }
    for (const auto &neq : disequalities_) {
      ext_use[findRepReadOnly(neq.first)]++;
      ext_use[findRepReadOnly(neq.second)]++;
    }
    for (const auto &op : active_arith_ops) {
      arith_use[findRepReadOnly(op.result)]++;
      arith_use[findRepReadOnly(op.op1)]++;
      arith_use[findRepReadOnly(op.op2)]++;
    }
    auto hasAnyUnary = [&](AbstractValue v) -> bool {
      return lower_bounds_.count(v) > 0 || upper_bounds_.count(v) > 0 ||
             null_values_.count(v) > 0 || non_null_values_.count(v) > 0;
    };
    auto isSingleUseInput = [&](AbstractValue v) -> bool {
      return ext_use[v] == 0 && arith_use[v] == 1;
    };
    auto satisfiesUnary = [&](AbstractValue r, int64_t val) -> bool {
      if (null_values_.count(r) > 0 && val != 0)
        return false;
      if (non_null_values_.count(r) > 0 && val == 0)
        return false;
      auto lo = lower_bounds_.find(r);
      if (lo != lower_bounds_.end() && val < lo->second)
        return false;
      auto hi = upper_bounds_.find(r);
      if (hi != upper_bounds_.end() && val > hi->second)
        return false;
      return true;
    };
    auto isFreeInput = [&](AbstractValue v) -> bool {
      return isSingleUseInput(v) && !hasAnyUnary(v);
    };

    std::vector<bool> peeled(active_arith_ops.size(), false);
    bool progress = true;
    while (progress) {
      progress = false;
      for (size_t i = 0; i < active_arith_ops.size(); ++i) {
        if (peeled[i])
          continue;
        const auto &op = active_arith_ops[i];
        AbstractValue r = findRepReadOnly(op.result);
        AbstractValue o1 = findRepReadOnly(op.op1);
        AbstractValue o2 = findRepReadOnly(op.op2);
        if (r == o1 || r == o2 || o1 == o2)
          continue;
        if (ext_use[r] != 0 || arith_use[r] != 1)
          continue;
        const bool f1 = isFreeInput(o1);
        const bool f2 = isFreeInput(o2);
        const bool s1 = isSingleUseInput(o1);
        const bool s2 = isSingleUseInput(o2);
        if (!s1 && !s2)
          continue;

        bool trivially_sat = false;
        if (op.op == "+" || op.op == "-" || op.op == "^") {
          if (f1 || f2) {
            // Bijective in any unconstrained operand over 64-bit integers.
            trivially_sat = true;
          } else if (s1) {
            if (auto c2 = asConstantInt(o2)) {
              int64_t cands[] = {0, 1, -1, *c2, *c2 ^ 1};
              for (int64_t x : cands) {
                if (!satisfiesUnary(o1, x))
                  continue;
                int64_t res = (op.op == "+")
                                  ? static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(*c2))
                                  : (op.op == "-"
                                         ? static_cast<int64_t>(static_cast<uint64_t>(x) - static_cast<uint64_t>(*c2))
                                         : (x ^ *c2));
                if (satisfiesUnary(r, res)) {
                  trivially_sat = true;
                  break;
                }
              }
            }
          }
        } else if (op.op == "&") {
          if (f1 && f2) {
            trivially_sat = true;
          } else {
            auto checkAnd = [&](AbstractValue var_op, AbstractValue const_op) {
              if (!isSingleUseInput(var_op))
                return false;
              auto c = asConstantInt(const_op);
              if (!c)
                return false;
              int64_t cands[] = {0, *c, -1, 1, ~(*c)};
              for (int64_t x : cands) {
                if (satisfiesUnary(var_op, x) && satisfiesUnary(r, x & *c)) {
                  return true;
                }
              }
              return false;
            };
            if (checkAnd(o1, o2) || checkAnd(o2, o1)) {
              trivially_sat = true;
            }
          }
        } else if (op.op == "|") {
          if (f1 && f2) {
            trivially_sat = true;
          } else {
            auto checkOr = [&](AbstractValue var_op, AbstractValue const_op) {
              if (!isSingleUseInput(var_op))
                return false;
              auto c = asConstantInt(const_op);
              if (!c)
                return false;
              int64_t cands[] = {0, 1, -1, *c, *c | 1};
              for (int64_t x : cands) {
                if (satisfiesUnary(var_op, x) && satisfiesUnary(r, x | *c)) {
                  return true;
                }
              }
              return false;
            };
            if (checkOr(o1, o2) || checkOr(o2, o1)) {
              trivially_sat = true;
            }
          }
        } else if ((op.op == "<<" || op.op == ">>") && s1) {
          auto s = asConstantInt(o2);
          if (s && *s >= 0 && *s < 64) {
            unsigned sh = static_cast<unsigned>(*s);
            int64_t cands[] = {0, 1, static_cast<int64_t>(1ULL << sh), -1};
            for (int64_t x : cands) {
              if (!satisfiesUnary(o1, x))
                continue;
              int64_t res = (op.op == "<<")
                                ? static_cast<int64_t>(static_cast<uint64_t>(x) << sh)
                                : (x >> sh);
              if (satisfiesUnary(r, res)) {
                trivially_sat = true;
                break;
              }
            }
          }
        }

        if (trivially_sat) {
          peeled[i] = true;
          arith_use[r]--;
          arith_use[o1]--;
          arith_use[o2]--;
          progress = true;
        }
      }
    }
    std::vector<ArithmeticOp> remaining;
    remaining.reserve(active_arith_ops.size());
    for (size_t i = 0; i < active_arith_ops.size(); ++i) {
      if (!peeled[i])
        remaining.push_back(active_arith_ops[i]);
    }
    active_arith_ops.swap(remaining);
  }

  if (!needs_linear_smt && active_arith_ops.empty() && float_cmps_.empty() &&
      active_disj_eqs.empty()) {
    return cacheAndReturn(true);
  }

  // Compute the connected set of integer variables `z3_vars` that actually
  // participate in active relational / arithmetic / disjunctive constraints,
  // so we never dump hundreds of unrelated pointers/constants into Z3!
  std::set<AbstractValue> z3_vars;
  std::vector<bool> keep_linear(linear_constraints_.size(), false);
  if (needs_linear_smt) {
    for (size_t i = 0; i < linear_constraints_.size(); ++i) {
      keep_linear[i] = true;
      for (const auto &t : linear_constraints_[i].terms) {
        z3_vars.insert(findRepReadOnly(t.var));
      }
    }
  }
  for (const auto &op : active_arith_ops) {
    z3_vars.insert(findRepReadOnly(op.result));
    z3_vars.insert(findRepReadOnly(op.op1));
    z3_vars.insert(findRepReadOnly(op.op2));
  }
  for (const auto &deq : active_disj_eqs) {
    z3_vars.insert(findRepReadOnly(deq.target));
    for (AbstractValue ov : deq.var_options) {
      z3_vars.insert(findRepReadOnly(ov));
    }
  }

  auto hasUnaryConstraint = [&](AbstractValue c) -> bool {
    return lower_bounds_.count(c) > 0 || upper_bounds_.count(c) > 0 ||
           null_values_.count(c) > 0 || non_null_values_.count(c) > 0;
  };

  std::vector<std::pair<AbstractValue, AbstractValue>> active_diseqs;
  if (!z3_vars.empty()) {
    bool expanded = true;
    while (expanded) {
      expanded = false;
      for (size_t i = 0; i < linear_constraints_.size(); ++i) {
        if (keep_linear[i])
          continue;
        bool touches = false;
        for (const auto &t : linear_constraints_[i].terms) {
          if (z3_vars.count(findRepReadOnly(t.var)) > 0) {
            touches = true;
            break;
          }
        }
        if (touches) {
          keep_linear[i] = true;
          for (const auto &t : linear_constraints_[i].terms) {
            if (z3_vars.insert(findRepReadOnly(t.var)).second)
              expanded = true;
          }
        }
      }
      for (const auto &neq : disequalities_) {
        AbstractValue c1 = findRepReadOnly(neq.first);
        AbstractValue c2 = findRepReadOnly(neq.second);
        if (c1 == c2)
          return cacheAndReturn(false);
        const bool in1 = z3_vars.count(c1) > 0;
        const bool in2 = z3_vars.count(c2) > 0;
        if (in1 && !in2 && hasUnaryConstraint(c2)) {
          z3_vars.insert(c2);
          expanded = true;
        } else if (in2 && !in1 && hasUnaryConstraint(c1)) {
          z3_vars.insert(c1);
          expanded = true;
        }
      }
    }
    for (const auto &neq : disequalities_) {
      AbstractValue c1 = findRepReadOnly(neq.first);
      AbstractValue c2 = findRepReadOnly(neq.second);
      if (z3_vars.count(c1) > 0 && z3_vars.count(c2) > 0) {
        active_diseqs.emplace_back(c1, c2);
      }
    }
  }

  std::vector<const LinearConstraint *> active_linear;
  for (size_t i = 0; i < linear_constraints_.size(); ++i) {
    if (keep_linear[i]) {
      active_linear.push_back(&linear_constraints_[i]);
    }
  }

  // Bound Z3 query complexity on very large straight-line functions.
  if (active_linear.size() + active_arith_ops.size() > 64) {
    return cacheAndReturn(true);
  }

  try {
    z3::context &ctx = getCachedZ3Context();

    // Fast pure QF_BV (64-bit bitvector) solver when bitwise ops are present
    // and no floating-point constraints exist. Mixing unbounded Int with
    // int2bv/bv2int prevents bit-blasting and causes Z3 timeouts, whereas
    // pure QF_BV decides bitwise identities in <1ms.
    bool has_bitwise = false;
    bool has_nonlin_int = false;
    unsigned nonlin_op_count = 0;
    for (const auto &op : active_arith_ops) {
      if (op.op == "&" || op.op == "|" || op.op == "^" || op.op == "<<" ||
          op.op == ">>") {
        has_bitwise = true;
        has_nonlin_int = true;
        ++nonlin_op_count;
      } else if (op.op == "/" || op.op == "%") {
        // Division/mod by a non-zero constant is linear in QF_LIA; only
        // symbolic divisors require nonlinear reasoning.
        if (!asConstantInt(op.op2)) {
          has_nonlin_int = true;
          ++nonlin_op_count;
        }
      } else if (op.op == "*" && !asConstantInt(op.op1) &&
                 !asConstantInt(op.op2)) {
        has_nonlin_int = true;
        ++nonlin_op_count;
      }
    }
    auto buildNormalizedQueryKey = [&](char tag) -> std::string {
      std::unordered_map<AbstractValue, unsigned> norm_id;
      auto getNormId = [&](AbstractValue v) -> unsigned {
        AbstractValue c = findRepReadOnly(v);
        auto it = norm_id.find(c);
        if (it != norm_id.end())
          return it->second;
        unsigned id = static_cast<unsigned>(norm_id.size());
        norm_id.emplace(c, id);
        return id;
      };
      std::string key;
      key.reserve(256);
      key.push_back(tag);
      for (AbstractValue c : z3_vars) {
        unsigned id = getNormId(c);
        auto lit = lower_bounds_.find(c);
        if (lit != lower_bounds_.end()) {
          key += "L";
          key += std::to_string(id);
          key += "=";
          key += std::to_string(lit->second);
          key += ";";
        }
        auto uit = upper_bounds_.find(c);
        if (uit != upper_bounds_.end()) {
          key += "U";
          key += std::to_string(id);
          key += "=";
          key += std::to_string(uit->second);
          key += ";";
        }
        if (null_values_.count(c) > 0) {
          key += "Z";
          key += std::to_string(id);
          key += ";";
        }
        if (non_null_values_.count(c) > 0) {
          key += "N";
          key += std::to_string(id);
          key += ";";
        }
      }
      for (const auto &neq : active_diseqs) {
        key += "D";
        key += std::to_string(getNormId(neq.first));
        key += ",";
        key += std::to_string(getNormId(neq.second));
        key += ";";
      }
      for (const LinearConstraint *cp : active_linear) {
        key += "C";
        key += std::to_string(static_cast<int>(cp->kind));
        key += ":";
        key += std::to_string(cp->constant);
        for (const auto &t : cp->terms) {
          key += ",";
          key += std::to_string(t.coefficient);
          key += "*";
          key += std::to_string(getNormId(t.var));
        }
        key += ";";
      }
      for (const auto &aop : active_arith_ops) {
        key += "A";
        key += std::to_string(getNormId(aop.result));
        key += "=";
        key += std::to_string(getNormId(aop.op1));
        key += aop.op;
        key += std::to_string(getNormId(aop.op2));
        key += ";";
      }
      for (const auto &deq : active_disj_eqs) {
        key += "J";
        key += std::to_string(getNormId(deq.target));
        for (int64_t c : deq.const_options) {
          key += "#";
          key += std::to_string(c);
        }
        for (AbstractValue ov : deq.var_options) {
          key += "@";
          key += std::to_string(getNormId(ov));
        }
        key += ";";
      }
      return key;
    };

    static thread_local std::unordered_map<std::string, bool> smt_query_cache;

    if (has_bitwise && float_ops_.empty() && float_cmps_.empty()) {
      // Skip cryptographic / hash / CRC chains (> 6 active bitwise/arith ops
      // or > 8 linear constraints): 64-bit bit-blasting on deep chains always
      // times out and bloats Z3's AST manager.
      if (nonlin_op_count > 6 || active_arith_ops.size() > 8 ||
          active_linear.size() > 8 || active_disj_eqs.size() > 4) {
        return cacheAndReturn(true);
      }

      std::string query_key = buildNormalizedQueryKey('B');
      auto cit = smt_query_cache.find(query_key);
      if (cit != smt_query_cache.end()) {
        return cacheAndReturn(cit->second);
      }

      // Use Z3's bit-blasting QF_BV solver (with pooled variable names to avoid
      // AST/symbol growth and a tight 5ms timeout).
      z3::solver bv_solver(ctx, "QF_BV");
      z3::params bp(ctx);
      bp.set(":timeout", 5u);
      bv_solver.set(bp);
      std::unordered_map<AbstractValue, z3::expr> bv_map;
      auto getBV = [&](AbstractValue v) -> z3::expr {
        return getZ3BVVar(ctx, findRepReadOnly(v), bv_map);
      };
      for (AbstractValue c : z3_vars) {
        auto lit = lower_bounds_.find(c);
        if (lit != lower_bounds_.end()) {
          bv_solver.add(getBV(c) >= ctx.bv_val(lit->second, 64));
        }
        auto uit = upper_bounds_.find(c);
        if (uit != upper_bounds_.end()) {
          bv_solver.add(getBV(c) <= ctx.bv_val(uit->second, 64));
        }
        if (null_values_.count(c) > 0) {
          bv_solver.add(getBV(c) == ctx.bv_val(0, 64));
        }
        if (non_null_values_.count(c) > 0) {
          bv_solver.add(getBV(c) != ctx.bv_val(0, 64));
        }
      }
      for (const auto &neq : active_diseqs) {
        bv_solver.add(getBV(neq.first) != getBV(neq.second));
      }
      for (const LinearConstraint *cp : active_linear) {
        const auto &constraint = *cp;
        z3::expr lhs = ctx.bv_val(0, 64);
        for (const auto &term : constraint.terms) {
          lhs = lhs + (ctx.bv_val(term.coefficient, 64) * getBV(term.var));
        }
        z3::expr rhs = ctx.bv_val(constraint.constant, 64);
        switch (constraint.kind) {
        case ConstraintKind::LessEqual:
          bv_solver.add(lhs <= rhs);
          break;
        case ConstraintKind::Less:
          bv_solver.add(lhs < rhs);
          break;
        case ConstraintKind::GreaterEqual:
          bv_solver.add(lhs >= rhs);
          break;
        case ConstraintKind::Greater:
          bv_solver.add(lhs > rhs);
          break;
        case ConstraintKind::Equal:
          bv_solver.add(lhs == rhs);
          break;
        }
      }
      for (const auto &arith_op : active_arith_ops) {
        z3::expr rv = getBV(arith_op.result);
        z3::expr o1 = getBV(arith_op.op1);
        z3::expr o2 = getBV(arith_op.op2);
        if (arith_op.op == "+")
          bv_solver.add(rv == o1 + o2);
        else if (arith_op.op == "-")
          bv_solver.add(rv == o1 - o2);
        else if (arith_op.op == "*") {
          // Bit-blasting 64-bit symbolic*symbolic is exponential; only encode
          // when at least one factor is a known constant.
          if (asConstantInt(arith_op.op1) || asConstantInt(arith_op.op2)) {
            bv_solver.add(rv == o1 * o2);
          }
        } else if (arith_op.op == "/") {
          bv_solver.add(o2 != ctx.bv_val(0, 64));
          if (asConstantInt(arith_op.op1) && asConstantInt(arith_op.op2)) {
            bv_solver.add(rv == o1 / o2);
          }
        } else if (arith_op.op == "%") {
          bv_solver.add(o2 != ctx.bv_val(0, 64));
          if (asConstantInt(arith_op.op1) && asConstantInt(arith_op.op2)) {
            bv_solver.add(rv == z3::srem(o1, o2));
          }
        } else if (arith_op.op == "&")
          bv_solver.add(rv == (o1 & o2));
        else if (arith_op.op == "|")
          bv_solver.add(rv == (o1 | o2));
        else if (arith_op.op == "^")
          bv_solver.add(rv == (o1 ^ o2));
        else if (arith_op.op == "<<") {
          if (asConstantInt(arith_op.op2)) {
            bv_solver.add(rv == z3::shl(o1, o2));
          }
        } else if (arith_op.op == ">>") {
          if (asConstantInt(arith_op.op2)) {
            bv_solver.add(rv == z3::ashr(o1, o2));
          }
        }
      }
      for (const auto &deq : active_disj_eqs) {
        z3::expr tv = getBV(deq.target);
        z3::expr_vector alts(ctx);
        for (int64_t c : deq.const_options) {
          alts.push_back(tv == ctx.bv_val(c, 64));
        }
        for (AbstractValue ov : deq.var_options) {
          alts.push_back(tv == getBV(ov));
        }
        if (alts.size() > 0) {
          bv_solver.add(z3::mk_or(alts));
        }
      }
      const bool is_sat = (bv_solver.check() != z3::unsat);
      if (smt_query_cache.size() >= 4096) {
        smt_query_cache.clear();
      }
      smt_query_cache.emplace(std::move(query_key), is_sat);
      return cacheAndReturn(is_sat);
    }

    const bool has_floats = !float_cmps_.empty();
    bool has_nonlin_float = false;
    if (has_floats) {
      for (const auto &fop : float_ops_) {
        if (fop.op == "*" || fop.op == "/") {
          has_nonlin_float = true;
          break;
        }
      }
    }
    // Use the persistent lightweight simple solver for linear/QF_LIA/QF_LRA;
    // only instantiate a fresh tactic solver for small nonlinear queries.
    const bool use_nonlin = has_nonlin_int || has_nonlin_float;
    if (use_nonlin && (nonlin_op_count > 4 || active_arith_ops.size() > 8 ||
                       active_linear.size() > 8)) {
      return cacheAndReturn(true);
    }
    std::string nonlin_key;
    if (use_nonlin && !has_floats) {
      nonlin_key = buildNormalizedQueryKey('N');
      auto cit = smt_query_cache.find(nonlin_key);
      if (cit != smt_query_cache.end()) {
        return cacheAndReturn(cit->second);
      }
    }
    std::optional<z3::solver> nonlin_solver;
    std::optional<SolverPushScope> push_scope;
    z3::solver *solver_ptr = nullptr;
    if (!use_nonlin) {
      solver_ptr = &getCachedSimpleSolver(ctx);
      push_scope.emplace(*solver_ptr);
    } else {
      nonlin_solver.emplace(ctx);
      z3::params p(ctx);
      p.set(":timeout", 5u);
      nonlin_solver->set(p);
      solver_ptr = &(*nonlin_solver);
    }
    z3::solver &solver = *solver_ptr;
    std::unordered_map<AbstractValue, z3::expr> var_map;

    auto getCanon = [&](AbstractValue v) -> AbstractValue {
      return findRepReadOnly(v);
    };

    for (const LinearConstraint *cp : active_linear) {
      const auto &constraint = *cp;
      z3::expr lhs = ctx.int_val(0);
      for (const auto &term : constraint.terms) {
        AbstractValue canon = getCanon(term.var);
        z3::expr var = getZ3Var(ctx, canon, var_map);
        lhs = lhs + (ctx.int_val(term.coefficient) * var);
      }

      z3::expr rhs = ctx.int_val(constraint.constant);

      switch (constraint.kind) {
      case ConstraintKind::LessEqual:
        solver.add(lhs <= rhs);
        break;
      case ConstraintKind::Less:
        solver.add(lhs < rhs);
        break;
      case ConstraintKind::GreaterEqual:
        solver.add(lhs >= rhs);
        break;
      case ConstraintKind::Greater:
        solver.add(lhs > rhs);
        break;
      case ConstraintKind::Equal:
        solver.add(lhs == rhs);
        break;
      }
    }

    for (AbstractValue canon : z3_vars) {
      auto lit = lower_bounds_.find(canon);
      if (lit != lower_bounds_.end()) {
        solver.add(getZ3Var(ctx, canon, var_map) >= ctx.int_val(lit->second));
      }
      auto uit = upper_bounds_.find(canon);
      if (uit != upper_bounds_.end()) {
        solver.add(getZ3Var(ctx, canon, var_map) <= ctx.int_val(uit->second));
      }
      if (null_values_.count(canon) > 0) {
        solver.add(getZ3Var(ctx, canon, var_map) == ctx.int_val(0));
      }
      if (non_null_values_.count(canon) > 0) {
        solver.add(getZ3Var(ctx, canon, var_map) != ctx.int_val(0));
      }
    }

    for (const auto &neq : active_diseqs) {
      z3::expr v1 = getZ3Var(ctx, neq.first, var_map);
      z3::expr v2 = getZ3Var(ctx, neq.second, var_map);
      solver.add(v1 != v2);
    }

    for (const auto &arith_op : active_arith_ops) {
      AbstractValue canon_result = getCanon(arith_op.result);
      AbstractValue canon_op1 = getCanon(arith_op.op1);
      AbstractValue canon_op2 = getCanon(arith_op.op2);

      z3::expr result_var = getZ3Var(ctx, canon_result, var_map);
      z3::expr op1_var = getZ3Var(ctx, canon_op1, var_map);
      z3::expr op2_var = getZ3Var(ctx, canon_op2, var_map);

      if (arith_op.op == "+") {
        solver.add(result_var == op1_var + op2_var);
      } else if (arith_op.op == "-") {
        solver.add(result_var == op1_var - op2_var);
      } else if (arith_op.op == "*") {
        solver.add(result_var == op1_var * op2_var);
      } else if (arith_op.op == "/") {
        solver.add(op2_var != 0);
        if (auto c2 = asConstantInt(arith_op.op2)) {
          if (*c2 != 0)
            solver.add(result_var == op1_var / ctx.int_val(*c2));
        } else {
          solver.add(result_var == op1_var / op2_var);
        }
      } else if (arith_op.op == "%") {
        solver.add(op2_var != 0);
        if (auto c2 = asConstantInt(arith_op.op2)) {
          if (*c2 != 0)
            solver.add(result_var == z3::mod(op1_var, ctx.int_val(*c2)));
        } else {
          solver.add(result_var == z3::mod(op1_var, op2_var));
        }
      } else if (arith_op.op == "&") {
        solver.add(result_var ==
                   fromBV64(ctx, toBV64(ctx, op1_var) & toBV64(ctx, op2_var)));
      } else if (arith_op.op == "|") {
        solver.add(result_var ==
                   fromBV64(ctx, toBV64(ctx, op1_var) | toBV64(ctx, op2_var)));
      } else if (arith_op.op == "^") {
        solver.add(result_var ==
                   fromBV64(ctx, toBV64(ctx, op1_var) ^ toBV64(ctx, op2_var)));
      } else if (arith_op.op == "<<") {
        solver.add(result_var ==
                   fromBV64(ctx, z3::shl(toBV64(ctx, op1_var),
                                         toBV64(ctx, op2_var))));
      } else if (arith_op.op == ">>") {
        solver.add(result_var ==
                   fromBV64(ctx, z3::ashr(toBV64(ctx, op1_var),
                                          toBV64(ctx, op2_var))));
      } else {
        continue;
      }
    }

    // Disjunctive equalities from PulseJoin (v = c1 ∨ v = c2 / v = o1 ∨ v = o2).
    for (const auto &deq : active_disj_eqs) {
      z3::expr tv = getZ3Var(ctx, getCanon(deq.target), var_map);
      z3::expr_vector alts(ctx);
      for (int64_t c : deq.const_options) {
        alts.push_back(tv == ctx.int_val(c));
      }
      for (AbstractValue ov : deq.var_options) {
        alts.push_back(tv == getZ3Var(ctx, getCanon(ov), var_map));
      }
      if (alts.size() > 0) {
        solver.add(z3::mk_or(alts));
      }
    }

    // Floating-point constraints encoded in Z3 Real theory.
    if (has_floats) {
      std::unordered_map<AbstractValue, z3::expr> real_map;
      for (const auto &kv : float_constants_) {
        z3::expr rv = getZ3RealVar(ctx, getCanon(kv.first), real_map);
        solver.add(rv == z3RealVal(ctx, kv.second));
      }
      for (const auto &fop : float_ops_) {
        z3::expr res = getZ3RealVar(ctx, getCanon(fop.result), real_map);
        z3::expr o1 = getZ3RealVar(ctx, getCanon(fop.op1), real_map);
        z3::expr o2 = getZ3RealVar(ctx, getCanon(fop.op2), real_map);
        if (fop.op == "+")
          solver.add(res == o1 + o2);
        else if (fop.op == "-")
          solver.add(res == o1 - o2);
        else if (fop.op == "*")
          solver.add(res == o1 * o2);
        else if (fop.op == "/") {
          solver.add(o2 != ctx.real_val(0));
          solver.add(res == o1 / o2);
        }
      }
      for (const auto &fcmp : float_cmps_) {
        z3::expr o1 = getZ3RealVar(ctx, getCanon(fcmp.op1), real_map);
        z3::expr o2 = getZ3RealVar(ctx, getCanon(fcmp.op2), real_map);
        if (fcmp.pred == "==")
          solver.add(o1 == o2);
        else if (fcmp.pred == "!=")
          solver.add(o1 != o2);
        else if (fcmp.pred == "<")
          solver.add(o1 < o2);
        else if (fcmp.pred == "<=")
          solver.add(o1 <= o2);
        else if (fcmp.pred == ">")
          solver.add(o1 > o2);
        else if (fcmp.pred == ">=")
          solver.add(o1 >= o2);
      }
    }

    z3::check_result result = solver.check();
    const bool is_sat = (result != z3::unsat);
    if (!nonlin_key.empty()) {
      if (smt_query_cache.size() >= 4096) {
        smt_query_cache.clear();
      }
      smt_query_cache.emplace(std::move(nonlin_key), is_sat);
    }
    return cacheAndReturn(is_sat);

  } catch (const z3::exception &) {
    return cacheAndReturn(isConsistent());
  }
}

std::optional<bool> PulseFormula::impliesComparison(
    std::optional<AbstractValue> lhs_var, std::optional<int64_t> lhs_const,
    const std::string &op, std::optional<AbstractValue> rhs_var,
    std::optional<int64_t> rhs_const, bool allow_smt) const {
  if (is_contradiction_) {
    return true;
  }
  // Fast constant / bound / equality path.
  auto lc = lhs_const ? lhs_const
                      : (lhs_var ? asConstantInt(*lhs_var) : std::nullopt);
  auto rc = rhs_const ? rhs_const
                      : (rhs_var ? asConstantInt(*rhs_var) : std::nullopt);
  if (lc && rc) {
    if (op == "==")
      return *lc == *rc;
    if (op == "!=")
      return *lc != *rc;
    if (op == "<")
      return *lc < *rc;
    if (op == "<=")
      return *lc <= *rc;
    if (op == ">")
      return *lc > *rc;
    if (op == ">=")
      return *lc >= *rc;
  }

  auto alo = lc ? lc : (lhs_var ? getLowerBound(*lhs_var) : std::nullopt);
  auto ahi = lc ? lc : (lhs_var ? getUpperBound(*lhs_var) : std::nullopt);
  auto blo = rc ? rc : (rhs_var ? getLowerBound(*rhs_var) : std::nullopt);
  auto bhi = rc ? rc : (rhs_var ? getUpperBound(*rhs_var) : std::nullopt);
  if (op == "<" || op == "<=") {
    if (ahi && blo && (op == "<" ? *ahi < *blo : *ahi <= *blo))
      return true;
    if (alo && bhi && (op == "<" ? *alo >= *bhi : *alo > *bhi))
      return false;
  } else if (op == ">" || op == ">=") {
    if (alo && bhi && (op == ">" ? *alo > *bhi : *alo >= *bhi))
      return true;
    if (ahi && blo && (op == ">" ? *ahi <= *blo : *ahi < *blo))
      return false;
  } else if (op == "==" || op == "!=") {
    if ((ahi && blo && *ahi < *blo) || (bhi && alo && *bhi < *alo))
      return op == "!=";
  }

  if (lhs_var && rhs_var) {
    if (areEqual(*lhs_var, *rhs_var)) {
      if (op == "==" || op == "<=" || op == ">=")
        return true;
      return false;
    }
    if (areDisequal(*lhs_var, *rhs_var)) {
      if (op == "==")
        return false;
      if (op == "!=")
        return true;
    }
  }
  if (lhs_var && rc && *rc == 0 && isNonNull(*lhs_var)) {
    if (op == "==")
      return false;
    if (op == "!=")
      return true;
  }
  if (rhs_var && lc && *lc == 0 && isNonNull(*rhs_var)) {
    if (op == "==")
      return false;
    if (op == "!=")
      return true;
  }

  // SMT entailment fallback when relational or disjunctive constraints exist.
  if (!allow_smt || pulse::options::disableSMT() ||
      (linear_constraints_.empty() && arithmetic_ops_.empty() &&
       disjunctive_eqs_.empty()) ||
      linear_constraints_.size() + arithmetic_ops_.size() > 64) {
    return std::nullopt;
  }
  if ((!lhs_var && !lc) || (!rhs_var && !rc)) {
    return std::nullopt;
  }

  // Slice to the connected component of {lhs_var, rhs_var}: if neither operand
  // participates in any linear, arithmetic, or disjunctive constraint, the
  // unary/equality fast-paths above are already complete -> 0 Z3 calls!
  std::set<AbstractValue> rel_vars;
  if (lhs_var && !lc) {
    rel_vars.insert(findRepReadOnly(*lhs_var));
  }
  if (rhs_var && !rc) {
    rel_vars.insert(findRepReadOnly(*rhs_var));
  }
  if (rel_vars.empty()) {
    return std::nullopt;
  }

  std::vector<bool> keep_lin(linear_constraints_.size(), false);
  std::vector<bool> keep_arith(arithmetic_ops_.size(), false);
  std::vector<bool> keep_disj(disjunctive_eqs_.size(), false);
  auto hasUnary = [&](AbstractValue c) -> bool {
    return lower_bounds_.count(c) > 0 || upper_bounds_.count(c) > 0 ||
           null_values_.count(c) > 0 || non_null_values_.count(c) > 0;
  };
  bool expanded = true;
  while (expanded) {
    expanded = false;
    for (size_t i = 0; i < linear_constraints_.size(); ++i) {
      if (keep_lin[i])
        continue;
      bool touches = false;
      for (const auto &t : linear_constraints_[i].terms) {
        if (rel_vars.count(findRepReadOnly(t.var)) > 0) {
          touches = true;
          break;
        }
      }
      if (touches) {
        keep_lin[i] = true;
        for (const auto &t : linear_constraints_[i].terms) {
          if (rel_vars.insert(findRepReadOnly(t.var)).second)
            expanded = true;
        }
      }
    }
    for (size_t i = 0; i < arithmetic_ops_.size(); ++i) {
      if (keep_arith[i])
        continue;
      AbstractValue r = findRepReadOnly(arithmetic_ops_[i].result);
      AbstractValue o1 = findRepReadOnly(arithmetic_ops_[i].op1);
      AbstractValue o2 = findRepReadOnly(arithmetic_ops_[i].op2);
      if (rel_vars.count(r) > 0 || rel_vars.count(o1) > 0 ||
          rel_vars.count(o2) > 0) {
        keep_arith[i] = true;
        if (rel_vars.insert(r).second)
          expanded = true;
        if (rel_vars.insert(o1).second)
          expanded = true;
        if (rel_vars.insert(o2).second)
          expanded = true;
      }
    }
    for (size_t i = 0; i < disjunctive_eqs_.size(); ++i) {
      if (keep_disj[i])
        continue;
      bool touches = rel_vars.count(findRepReadOnly(disjunctive_eqs_[i].target)) > 0;
      if (!touches) {
        for (AbstractValue ov : disjunctive_eqs_[i].var_options) {
          if (rel_vars.count(findRepReadOnly(ov)) > 0) {
            touches = true;
            break;
          }
        }
      }
      if (touches) {
        keep_disj[i] = true;
        if (rel_vars.insert(findRepReadOnly(disjunctive_eqs_[i].target)).second)
          expanded = true;
        for (AbstractValue ov : disjunctive_eqs_[i].var_options) {
          if (rel_vars.insert(findRepReadOnly(ov)).second)
            expanded = true;
        }
      }
    }
    for (const auto &neq : disequalities_) {
      AbstractValue c1 = findRepReadOnly(neq.first);
      AbstractValue c2 = findRepReadOnly(neq.second);
      const bool in1 = rel_vars.count(c1) > 0;
      const bool in2 = rel_vars.count(c2) > 0;
      if (in1 && !in2 && hasUnary(c2)) {
        rel_vars.insert(c2);
        expanded = true;
      } else if (in2 && !in1 && hasUnary(c1)) {
        rel_vars.insert(c1);
        expanded = true;
      }
    }
  }

  std::vector<const LinearConstraint *> slice_linear;
  for (size_t i = 0; i < linear_constraints_.size(); ++i) {
    if (keep_lin[i])
      slice_linear.push_back(&linear_constraints_[i]);
  }
  std::vector<const ArithmeticOp *> slice_arith;
  for (size_t i = 0; i < arithmetic_ops_.size(); ++i) {
    if (keep_arith[i])
      slice_arith.push_back(&arithmetic_ops_[i]);
  }
  std::vector<const DisjunctiveEq *> slice_disj;
  for (size_t i = 0; i < disjunctive_eqs_.size(); ++i) {
    if (keep_disj[i])
      slice_disj.push_back(&disjunctive_eqs_[i]);
  }
  if (slice_linear.empty() && slice_arith.empty() && slice_disj.empty()) {
    return std::nullopt;
  }

  std::vector<std::pair<AbstractValue, AbstractValue>> slice_diseqs;
  for (const auto &neq : disequalities_) {
    AbstractValue c1 = findRepReadOnly(neq.first);
    AbstractValue c2 = findRepReadOnly(neq.second);
    if (c1 == c2)
      return std::nullopt;
    if (rel_vars.count(c1) > 0 && rel_vars.count(c2) > 0) {
      slice_diseqs.emplace_back(c1, c2);
    }
  }

  try {
    z3::context &ctx = getCachedZ3Context();

    bool has_bitwise = false;
    bool has_nonlin_int = false;
    for (const ArithmeticOp *op : slice_arith) {
      if (op->op == "&" || op->op == "|" || op->op == "^" || op->op == "<<" ||
          op->op == ">>") {
        has_bitwise = true;
        has_nonlin_int = true;
      } else if (op->op == "/" || op->op == "%") {
        has_nonlin_int = true;
      } else if (op->op == "*" && !asConstantInt(op->op1) &&
                 !asConstantInt(op->op2)) {
        has_nonlin_int = true;
      }
    }
    if (has_bitwise && float_ops_.empty() && float_cmps_.empty()) {
      // Use Z3's bit-blasting QF_BV solver (with pooled variable names to avoid
      // AST/symbol growth and a tight 10ms timeout).
      z3::solver bv_solver(ctx, "QF_BV");
      z3::params bp(ctx);
      bp.set(":timeout", 10u);
      bv_solver.set(bp);
      std::unordered_map<AbstractValue, z3::expr> bv_map;
      auto getBV = [&](AbstractValue v) -> z3::expr {
        return getZ3BVVar(ctx, findRepReadOnly(v), bv_map);
      };
      for (AbstractValue c : rel_vars) {
        auto lit = lower_bounds_.find(c);
        if (lit != lower_bounds_.end()) {
          bv_solver.add(getBV(c) >= ctx.bv_val(lit->second, 64));
        }
        auto uit = upper_bounds_.find(c);
        if (uit != upper_bounds_.end()) {
          bv_solver.add(getBV(c) <= ctx.bv_val(uit->second, 64));
        }
        if (null_values_.count(c) > 0) {
          bv_solver.add(getBV(c) == ctx.bv_val(0, 64));
        }
        if (non_null_values_.count(c) > 0) {
          bv_solver.add(getBV(c) != ctx.bv_val(0, 64));
        }
      }
      for (const auto &neq : slice_diseqs) {
        bv_solver.add(getBV(neq.first) != getBV(neq.second));
      }
      for (const LinearConstraint *cp : slice_linear) {
        const auto &constraint = *cp;
        z3::expr lhs = ctx.bv_val(0, 64);
        for (const auto &term : constraint.terms) {
          lhs = lhs + (ctx.bv_val(term.coefficient, 64) * getBV(term.var));
        }
        z3::expr rhs = ctx.bv_val(constraint.constant, 64);
        switch (constraint.kind) {
        case ConstraintKind::LessEqual:
          bv_solver.add(lhs <= rhs);
          break;
        case ConstraintKind::Less:
          bv_solver.add(lhs < rhs);
          break;
        case ConstraintKind::GreaterEqual:
          bv_solver.add(lhs >= rhs);
          break;
        case ConstraintKind::Greater:
          bv_solver.add(lhs > rhs);
          break;
        case ConstraintKind::Equal:
          bv_solver.add(lhs == rhs);
          break;
        }
      }
      for (const ArithmeticOp *aop : slice_arith) {
        const auto &arith_op = *aop;
        z3::expr rv = getBV(arith_op.result);
        z3::expr o1 = getBV(arith_op.op1);
        z3::expr o2 = getBV(arith_op.op2);
        if (arith_op.op == "+")
          bv_solver.add(rv == o1 + o2);
        else if (arith_op.op == "-")
          bv_solver.add(rv == o1 - o2);
        else if (arith_op.op == "*")
          bv_solver.add(rv == o1 * o2);
        else if (arith_op.op == "/") {
          bv_solver.add(o2 != ctx.bv_val(0, 64));
          bv_solver.add(rv == o1 / o2);
        } else if (arith_op.op == "%") {
          bv_solver.add(o2 != ctx.bv_val(0, 64));
          bv_solver.add(rv == z3::srem(o1, o2));
        } else if (arith_op.op == "&")
          bv_solver.add(rv == (o1 & o2));
        else if (arith_op.op == "|")
          bv_solver.add(rv == (o1 | o2));
        else if (arith_op.op == "^")
          bv_solver.add(rv == (o1 ^ o2));
        else if (arith_op.op == "<<")
          bv_solver.add(rv == z3::shl(o1, o2));
        else if (arith_op.op == ">>")
          bv_solver.add(rv == z3::ashr(o1, o2));
      }
      for (const DisjunctiveEq *dp : slice_disj) {
        z3::expr tv = getBV(dp->target);
        z3::expr_vector alts(ctx);
        for (int64_t c : dp->const_options) {
          alts.push_back(tv == ctx.bv_val(c, 64));
        }
        for (AbstractValue ov : dp->var_options) {
          alts.push_back(tv == getBV(ov));
        }
        if (alts.size() > 0) {
          bv_solver.add(z3::mk_or(alts));
        }
      }
      z3::expr bv_le =
          lc ? ctx.bv_val(*lc, 64) : getBV(findRepReadOnly(*lhs_var));
      z3::expr bv_re =
          rc ? ctx.bv_val(*rc, 64) : getBV(findRepReadOnly(*rhs_var));
      z3::expr bv_pred = ctx.bool_val(true);
      if (op == "==")
        bv_pred = (bv_le == bv_re);
      else if (op == "!=")
        bv_pred = (bv_le != bv_re);
      else if (op == "<")
        bv_pred = (bv_le < bv_re);
      else if (op == "<=")
        bv_pred = (bv_le <= bv_re);
      else if (op == ">")
        bv_pred = (bv_le > bv_re);
      else if (op == ">=")
        bv_pred = (bv_le >= bv_re);
      else
        return std::nullopt;

      {
        SolverPushScope inner(bv_solver);
        bv_solver.add(!bv_pred);
        if (bv_solver.check() == z3::unsat)
          return true;
      }
      {
        SolverPushScope inner(bv_solver);
        bv_solver.add(bv_pred);
        if (bv_solver.check() == z3::unsat)
          return false;
      }
      return std::nullopt;
    }

    std::optional<z3::solver> nonlin_solver;
    std::optional<SolverPushScope> push_scope;
    z3::solver *solver_ptr = nullptr;
    if (!has_nonlin_int) {
      solver_ptr = &getCachedSimpleSolver(ctx);
      push_scope.emplace(*solver_ptr);
    } else {
      nonlin_solver.emplace(ctx);
      z3::params p(ctx);
      p.set(":timeout", 15u);
      nonlin_solver->set(p);
      solver_ptr = &(*nonlin_solver);
    }
    z3::solver &solver = *solver_ptr;
    std::unordered_map<AbstractValue, z3::expr> var_map;

    for (const LinearConstraint *cp : slice_linear) {
      const auto &constraint = *cp;
      z3::expr lhs_e = ctx.int_val(0);
      for (const auto &term : constraint.terms) {
        lhs_e = lhs_e + (ctx.int_val(term.coefficient) *
                         getZ3Var(ctx, findRepReadOnly(term.var), var_map));
      }
      z3::expr rhs_e = ctx.int_val(constraint.constant);
      switch (constraint.kind) {
      case ConstraintKind::LessEqual:
        solver.add(lhs_e <= rhs_e);
        break;
      case ConstraintKind::Less:
        solver.add(lhs_e < rhs_e);
        break;
      case ConstraintKind::GreaterEqual:
        solver.add(lhs_e >= rhs_e);
        break;
      case ConstraintKind::Greater:
        solver.add(lhs_e > rhs_e);
        break;
      case ConstraintKind::Equal:
        solver.add(lhs_e == rhs_e);
        break;
      }
    }
    for (AbstractValue c : rel_vars) {
      auto lit = lower_bounds_.find(c);
      if (lit != lower_bounds_.end()) {
        solver.add(getZ3Var(ctx, c, var_map) >= ctx.int_val(lit->second));
      }
      auto uit = upper_bounds_.find(c);
      if (uit != upper_bounds_.end()) {
        solver.add(getZ3Var(ctx, c, var_map) <= ctx.int_val(uit->second));
      }
      if (null_values_.count(c) > 0) {
        solver.add(getZ3Var(ctx, c, var_map) == ctx.int_val(0));
      }
      if (non_null_values_.count(c) > 0) {
        solver.add(getZ3Var(ctx, c, var_map) != ctx.int_val(0));
      }
    }
    for (const auto &neq : slice_diseqs) {
      solver.add(getZ3Var(ctx, neq.first, var_map) !=
                 getZ3Var(ctx, neq.second, var_map));
    }
    for (const ArithmeticOp *aop : slice_arith) {
      const auto &arith_op = *aop;
      z3::expr rv = getZ3Var(ctx, findRepReadOnly(arith_op.result), var_map);
      z3::expr o1 = getZ3Var(ctx, findRepReadOnly(arith_op.op1), var_map);
      z3::expr o2 = getZ3Var(ctx, findRepReadOnly(arith_op.op2), var_map);
      if (arith_op.op == "+")
        solver.add(rv == o1 + o2);
      else if (arith_op.op == "-")
        solver.add(rv == o1 - o2);
      else if (arith_op.op == "*")
        solver.add(rv == o1 * o2);
      else if (arith_op.op == "/") {
        solver.add(o2 != 0);
        solver.add(rv == o1 / o2);
      } else if (arith_op.op == "%") {
        solver.add(o2 != 0);
        solver.add(rv == z3::mod(o1, o2));
      } else if (arith_op.op == "&") {
        solver.add(rv == fromBV64(ctx, toBV64(ctx, o1) & toBV64(ctx, o2)));
      } else if (arith_op.op == "|") {
        solver.add(rv == fromBV64(ctx, toBV64(ctx, o1) | toBV64(ctx, o2)));
      } else if (arith_op.op == "^") {
        solver.add(rv == fromBV64(ctx, toBV64(ctx, o1) ^ toBV64(ctx, o2)));
      } else if (arith_op.op == "<<") {
        solver.add(rv == fromBV64(ctx, z3::shl(toBV64(ctx, o1),
                                               toBV64(ctx, o2))));
      } else if (arith_op.op == ">>") {
        solver.add(rv == fromBV64(ctx, z3::ashr(toBV64(ctx, o1),
                                                toBV64(ctx, o2))));
      }
    }
    for (const DisjunctiveEq *dp : slice_disj) {
      z3::expr tv = getZ3Var(ctx, findRepReadOnly(dp->target), var_map);
      z3::expr_vector alts(ctx);
      for (int64_t c : dp->const_options) {
        alts.push_back(tv == ctx.int_val(c));
      }
      for (AbstractValue ov : dp->var_options) {
        alts.push_back(tv == getZ3Var(ctx, findRepReadOnly(ov), var_map));
      }
      if (alts.size() > 0) {
        solver.add(z3::mk_or(alts));
      }
    }

    z3::expr le = lc ? ctx.int_val(*lc)
                     : getZ3Var(ctx, findRepReadOnly(*lhs_var), var_map);
    z3::expr re = rc ? ctx.int_val(*rc)
                     : getZ3Var(ctx, findRepReadOnly(*rhs_var), var_map);

    z3::expr pred = ctx.bool_val(true);
    if (op == "==")
      pred = (le == re);
    else if (op == "!=")
      pred = (le != re);
    else if (op == "<")
      pred = (le < re);
    else if (op == "<=")
      pred = (le <= re);
    else if (op == ">")
      pred = (le > re);
    else if (op == ">=")
      pred = (le >= re);
    else
      return std::nullopt;

    {
      SolverPushScope inner(solver);
      solver.add(!pred);
      if (solver.check() == z3::unsat) {
        return true;
      }
    }
    {
      SolverPushScope inner(solver);
      solver.add(pred);
      if (solver.check() == z3::unsat) {
        return false;
      }
    }
  } catch (const z3::exception &) {
    return std::nullopt;
  }
  return std::nullopt;
}

void PulseFormula::simplify() { propagateBounds(); }

void PulseFormula::propagateBounds() {
  for (const auto &eq : equalities_) {
    AbstractValue rep1 = findRep(eq.first);
    AbstractValue rep2 = findRep(eq.second);

    if (rep1 == rep2)
      continue;

    auto lower1 = lower_bounds_.find(rep1);
    auto upper1 = upper_bounds_.find(rep1);
    auto lower2 = lower_bounds_.find(rep2);
    auto upper2 = upper_bounds_.find(rep2);

    if (lower1 != lower_bounds_.end() || lower2 != lower_bounds_.end()) {
      int64_t new_lower = std::numeric_limits<int64_t>::min();
      if (lower1 != lower_bounds_.end())
        new_lower = std::max(new_lower, lower1->second);
      if (lower2 != lower_bounds_.end())
        new_lower = std::max(new_lower, lower2->second);
      if (new_lower != std::numeric_limits<int64_t>::min()) {
        lower_bounds_[rep1] = new_lower;
        lower_bounds_[rep2] = new_lower;
      }
    }

    if (upper1 != upper_bounds_.end() || upper2 != upper_bounds_.end()) {
      int64_t new_upper = std::numeric_limits<int64_t>::max();
      if (upper1 != upper_bounds_.end())
        new_upper = std::min(new_upper, upper1->second);
      if (upper2 != upper_bounds_.end())
        new_upper = std::min(new_upper, upper2->second);
      if (new_upper != std::numeric_limits<int64_t>::max()) {
        upper_bounds_[rep1] = new_upper;
        upper_bounds_[rep2] = new_upper;
      }
    }
  }
}

bool PulseFormula::addArithmeticOperation(AbstractValue result,
                                          AbstractValue op1, AbstractValue op2,
                                          const std::string &op) {
  if (op == "*")
    return addMultiply(result, op1, op2);
  if (op == "/")
    return addDivide(result, op1, op2);
  if (op == "%")
    return addModulo(result, op1, op2);
  if (op == "&")
    return addBitwiseAnd(result, op1, op2);
  if (op == "|")
    return addBitwiseOr(result, op1, op2);
  if (op == "^")
    return addBitwiseXor(result, op1, op2);
  if (op == "<<")
    return addLeftShift(result, op1, op2);
  if (op == ">>")
    return addRightShift(result, op1, op2);

  if (op == "+" || op == "-") {
    // Constant fold when both operands have exact constant bounds.
    auto c1 = asConstantInt(op1);
    auto c2 = asConstantInt(op2);
    if (c1 && c2) {
      int64_t folded = (op == "+") ? satAddI64(*c1, *c2) : satAddI64(*c1, -*c2);
      if (!addBounds(result, folded, folded)) {
        return false;
      }
    }
    // Algebraic identities: x + 0 = x, x - 0 = x, x - x = 0.
    if (c2 && *c2 == 0) {
      if (!addEquality(result, op1))
        return false;
    } else if (op == "+" && c1 && *c1 == 0) {
      if (!addEquality(result, op2))
        return false;
    } else if (op == "-" && areEqual(op1, op2)) {
      if (!addBounds(result, 0, 0))
        return false;
    }

    LinearConstraint constraint;
    constraint.terms.emplace_back(result, 1);
    constraint.terms.emplace_back(op1, -1);
    constraint.terms.emplace_back(op2, op == "+" ? -1 : 1);
    constraint.constant = 0;
    constraint.kind = ConstraintKind::Equal;
    return addLinearConstraint(constraint);
  }

  return false;
}

bool PulseFormula::addMultiply(AbstractValue result, AbstractValue op1,
                               AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  auto c1 = asConstantInt(op1);
  auto c2 = asConstantInt(op2);
  if (c1 && c2) {
    int64_t prod = satMulI64(*c1, *c2);
    if (!addBounds(result, prod, prod))
      return false;
  } else if ((c1 && *c1 == 0) || (c2 && *c2 == 0)) {
    if (!addBounds(result, 0, 0))
      return false;
  } else if (c1 && *c1 == 1) {
    if (!addEquality(result, op2))
      return false;
  } else if (c2 && *c2 == 1) {
    if (!addEquality(result, op1))
      return false;
  } else if (c1) {
    // Linear scaling: result - c1 * op2 == 0
    LinearEqRow row;
    row.coeffs[findRep(result)] = 1;
    row.coeffs[findRep(op2)] =
        satAddI64(row.coeffs[findRep(op2)], -(*c1));
    if (row.coeffs[findRep(op2)] == 0)
      row.coeffs.erase(findRep(op2));
    if (!insertLinearEquationRow(std::move(row)))
      return false;
  } else if (c2) {
    // Linear scaling: result - c2 * op1 == 0
    LinearEqRow row;
    row.coeffs[findRep(result)] = 1;
    row.coeffs[findRep(op1)] =
        satAddI64(row.coeffs[findRep(op1)], -(*c2));
    if (row.coeffs[findRep(op1)] == 0)
      row.coeffs.erase(findRep(op1));
    if (!insertLinearEquationRow(std::move(row)))
      return false;
  }
  constexpr size_t kMaxArithOps = 128;
  if (arithmetic_ops_.size() < kMaxArithOps) {
    invalidateSatOnly();
    ArithmeticOp op{result, op1, op2, "*"};
    arithmetic_ops_.push_back(op);
  }
  return in_tableau_propagate_ ? true : isConsistent();
}

bool PulseFormula::addDivide(AbstractValue result, AbstractValue op1,
                             AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  auto c2 = asConstantInt(op2);
  if (isNull(op2) || (c2 && *c2 == 0)) {
    return false; // Division by zero
  }
  auto c1 = asConstantInt(op1);
  if (c1 && c2 && !(*c1 == std::numeric_limits<int64_t>::min() && *c2 == -1)) {
    int64_t q = (*c1) / (*c2);
    if (!addBounds(result, q, q))
      return false;
  } else if (c2 && *c2 == 1) {
    if (!addEquality(result, op1))
      return false;
  }
  constexpr size_t kMaxArithOps = 128;
  if (arithmetic_ops_.size() < kMaxArithOps) {
    invalidateSatOnly();
    ArithmeticOp op{result, op1, op2, "/"};
    arithmetic_ops_.push_back(op);
  }
  return in_tableau_propagate_ ? true : isConsistent();
}

bool PulseFormula::addModulo(AbstractValue result, AbstractValue op1,
                             AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  auto c2 = asConstantInt(op2);
  if (isNull(op2) || (c2 && *c2 == 0)) {
    return false; // Modulo by zero
  }
  auto c1 = asConstantInt(op1);
  if (c1 && c2 && !(*c1 == std::numeric_limits<int64_t>::min() && *c2 == -1)) {
    int64_t r = (*c1) % (*c2);
    if (!addBounds(result, r, r))
      return false;
  }
  constexpr size_t kMaxArithOps = 128;
  if (arithmetic_ops_.size() < kMaxArithOps) {
    invalidateSatOnly();
    ArithmeticOp op{result, op1, op2, "%"};
    arithmetic_ops_.push_back(op);
  }
  return in_tableau_propagate_ ? true : isConsistent();
}

bool PulseFormula::addBitwiseAnd(AbstractValue result, AbstractValue op1,
                                 AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  addIntegerConstraint(result);
  addIntegerConstraint(op1);
  addIntegerConstraint(op2);
  auto c1 = asConstantInt(op1);
  auto c2 = asConstantInt(op2);
  if (c1 && c2) {
    int64_t val = (*c1) & (*c2);
    return addBounds(result, val, val);
  }
  if ((c1 && *c1 == 0) || (c2 && *c2 == 0)) {
    return addBounds(result, 0, 0);
  }
  if (areEqual(op1, op2)) {
    return addEquality(result, op1);
  }
  if (c1 && *c1 >= 0) {
    if (!addBounds(result, 0, *c1))
      return false;
  } else if (c2 && *c2 >= 0) {
    if (!addBounds(result, 0, *c2))
      return false;
  }
  constexpr size_t kMaxArithOps = 128;
  if (arithmetic_ops_.size() < kMaxArithOps) {
    invalidateSatOnly();
    arithmetic_ops_.push_back(ArithmeticOp{result, op1, op2, "&"});
  }
  return in_tableau_propagate_ ? true : isConsistent();
}

bool PulseFormula::addBitwiseOr(AbstractValue result, AbstractValue op1,
                                AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  addIntegerConstraint(result);
  addIntegerConstraint(op1);
  addIntegerConstraint(op2);
  auto c1 = asConstantInt(op1);
  auto c2 = asConstantInt(op2);
  if (c1 && c2) {
    int64_t val = (*c1) | (*c2);
    return addBounds(result, val, val);
  }
  if (c1 && *c1 == 0)
    return addEquality(result, op2);
  if (c2 && *c2 == 0)
    return addEquality(result, op1);
  if (areEqual(op1, op2))
    return addEquality(result, op1);
  if ((c1 && *c1 != 0) || (c2 && *c2 != 0)) {
    if (!addNonNull(result))
      return false;
  }
  constexpr size_t kMaxArithOps = 128;
  if (arithmetic_ops_.size() < kMaxArithOps) {
    invalidateSatOnly();
    arithmetic_ops_.push_back(ArithmeticOp{result, op1, op2, "|"});
  }
  return in_tableau_propagate_ ? true : isConsistent();
}

bool PulseFormula::addBitwiseXor(AbstractValue result, AbstractValue op1,
                                 AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  addIntegerConstraint(result);
  addIntegerConstraint(op1);
  addIntegerConstraint(op2);
  auto c1 = asConstantInt(op1);
  auto c2 = asConstantInt(op2);
  if (c1 && c2) {
    int64_t val = (*c1) ^ (*c2);
    return addBounds(result, val, val);
  }
  if (c1 && *c1 == 0)
    return addEquality(result, op2);
  if (c2 && *c2 == 0)
    return addEquality(result, op1);
  if (areEqual(op1, op2))
    return addBounds(result, 0, 0);
  constexpr size_t kMaxArithOps = 128;
  if (arithmetic_ops_.size() < kMaxArithOps) {
    invalidateSatOnly();
    arithmetic_ops_.push_back(ArithmeticOp{result, op1, op2, "^"});
  }
  return in_tableau_propagate_ ? true : isConsistent();
}

bool PulseFormula::addLeftShift(AbstractValue result, AbstractValue op1,
                                AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  addIntegerConstraint(result);
  addIntegerConstraint(op1);
  addIntegerConstraint(op2);
  auto c1 = asConstantInt(op1);
  auto c2 = asConstantInt(op2);
  if (c1 && c2 && *c2 >= 0 && *c2 < 63) {
    uint64_t shifted = static_cast<uint64_t>(*c1) << static_cast<unsigned>(*c2);
    int64_t val = static_cast<int64_t>(shifted);
    return addBounds(result, val, val);
  }
  if (c2 && *c2 == 0)
    return addEquality(result, op1);
  constexpr size_t kMaxArithOps = 128;
  if (arithmetic_ops_.size() < kMaxArithOps) {
    invalidateSatOnly();
    arithmetic_ops_.push_back(ArithmeticOp{result, op1, op2, "<<"});
  }
  return in_tableau_propagate_ ? true : isConsistent();
}

bool PulseFormula::addRightShift(AbstractValue result, AbstractValue op1,
                                 AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  addIntegerConstraint(result);
  addIntegerConstraint(op1);
  addIntegerConstraint(op2);
  auto c1 = asConstantInt(op1);
  auto c2 = asConstantInt(op2);
  if (c1 && c2 && *c2 >= 0 && *c2 < 63) {
    int64_t val = (*c1) >> static_cast<unsigned>(*c2);
    return addBounds(result, val, val);
  }
  if (c2 && *c2 == 0)
    return addEquality(result, op1);
  constexpr size_t kMaxArithOps = 128;
  if (arithmetic_ops_.size() < kMaxArithOps) {
    invalidateSatOnly();
    arithmetic_ops_.push_back(ArithmeticOp{result, op1, op2, ">>"});
  }
  return in_tableau_propagate_ ? true : isConsistent();
}

//===----------------------------------------------------------------------===//
// Incremental RREF Linear-Arithmetic Tableau (Infer PulseFormulaTableau)
//===----------------------------------------------------------------------===//

bool PulseFormula::insertLinearEquationRow(LinearEqRow raw_row) {
  if (is_contradiction_) {
    return false;
  }

  // Build exact __int128 accumulator over canonical representatives.
  std::map<AbstractValue, __int128> acc;
  __int128 cst = raw_row.constant;

  for (const auto &kv : raw_row.coeffs) {
    if (kv.second == 0)
      continue;
    AbstractValue rep = findRep(kv.first);
    if (auto c = asConstantInt(rep)) {
      cst += static_cast<__int128>(kv.second) * static_cast<__int128>(*c);
    } else {
      acc[rep] += kv.second;
      if (acc[rep] == 0)
        acc.erase(rep);
    }
  }

  auto reduceAcc = [](std::map<AbstractValue, __int128> &m, __int128 &c) {
    __int128 g = abs128(c);
    for (const auto &kv : m) {
      g = (g == 0) ? abs128(kv.second) : gcd128(g, kv.second);
    }
    if (g > 1) {
      c /= g;
      for (auto &kv : m) {
        kv.second /= g;
      }
    }
  };

  // Eliminate any variable that is already a pivot in linear_tableau_.
  bool eliminated = true;
  unsigned elim_steps = 0;
  while (eliminated && elim_steps++ < 32) {
    eliminated = false;
    AbstractValue found_pivot;
    bool has_pivot = false;
    for (const auto &kv : acc) {
      if (linear_tableau_.find(kv.first) != linear_tableau_.end()) {
        found_pivot = kv.first;
        has_pivot = true;
        break;
      }
    }
    if (!has_pivot)
      break;

    const LinearEqRow &prow = linear_tableau_.at(found_pivot);
    auto pit = prow.coeffs.find(found_pivot);
    if (pit == prow.coeffs.end() || pit->second == 0) {
      break;
    }
    __int128 a = acc[found_pivot];
    __int128 b = pit->second; // > 0
    __int128 g = gcd128(a, b);
    __int128 mul_acc = b / g;
    __int128 mul_prow = a / g;

    // Scale acc by mul_acc and subtract mul_prow * prow.
    cst = cst * mul_acc - mul_prow * static_cast<__int128>(prow.constant);
    for (auto &kv : acc) {
      kv.second *= mul_acc;
    }
    for (const auto &kv : prow.coeffs) {
      AbstractValue v = findRep(kv.first);
      if (auto c = asConstantInt(v)) {
        cst -= mul_prow * static_cast<__int128>(kv.second) *
               static_cast<__int128>(*c);
      } else {
        acc[v] -= mul_prow * static_cast<__int128>(kv.second);
        if (acc[v] == 0)
          acc.erase(v);
      }
    }
    acc.erase(found_pivot);
    reduceAcc(acc, cst);
    eliminated = true;
  }

  reduceAcc(acc, cst);

  // Case 0: 0 + cst == 0 -> contradiction if cst != 0.
  if (acc.empty()) {
    return cst == 0;
  }

  // Case 1: a*v + cst == 0 -> solve for v = -cst / a (eagerly surfaces NewEq!).
  if (acc.size() == 1) {
    AbstractValue v = acc.begin()->first;
    __int128 a = acc.begin()->second;
    if (cst % a != 0) {
      return false; // No integer solution!
    }
    __int128 sol = -cst / a;
    if (!fitsInI64(sol)) {
      return true;
    }
    int64_t val = static_cast<int64_t>(sol);
    return addBounds(v, val, val);
  }

  // Case 2: a*v1 - a*v2 == 0 -> eager equality v1 == v2!
  if (acc.size() == 2 && cst == 0) {
    auto it = acc.begin();
    AbstractValue v1 = it->first;
    __int128 c1 = it->second;
    ++it;
    AbstractValue v2 = it->first;
    __int128 c2 = it->second;
    if (c1 == -c2) {
      return addEquality(v1, v2);
    }
  }

  // Ensure coefficients fit in int64_t; bail conservatively if overflow occurs.
  if (!fitsInI64(cst)) {
    return true;
  }
  for (const auto &kv : acc) {
    if (!fitsInI64(kv.second)) {
      return true;
    }
  }

  // Pick pivot = largest AbstractValue so temporaries reduce to older vars.
  AbstractValue pivot = acc.rbegin()->first;
  if (acc[pivot] < 0) {
    cst = -cst;
    for (auto &kv : acc) {
      kv.second = -kv.second;
    }
  }

  LinearEqRow norm_row;
  norm_row.constant = static_cast<int64_t>(cst);
  for (const auto &kv : acc) {
    norm_row.coeffs[kv.first] = static_cast<int64_t>(kv.second);
  }

  // Check if another pivot row already expresses the exact same linear
  // combination (e.g., a - b - c == 0 and d - b - c == 0 => a == d!).
  auto sameNonPivotRhs = [&](AbstractValue p1, const LinearEqRow &r1,
                             AbstractValue p2, const LinearEqRow &r2) -> bool {
    if (r1.coeffs.size() != r2.coeffs.size() || r1.constant != r2.constant) {
      return false;
    }
    auto it1 = r1.coeffs.find(p1);
    auto it2 = r2.coeffs.find(p2);
    if (it1 == r1.coeffs.end() || it2 == r2.coeffs.end() ||
        it1->second != it2->second) {
      return false;
    }
    for (const auto &kv : r1.coeffs) {
      if (kv.first == p1)
        continue;
      auto oit = r2.coeffs.find(kv.first);
      if (oit == r2.coeffs.end() || oit->second != kv.second) {
        return false;
      }
    }
    return true;
  };

  for (const auto &kv : linear_tableau_) {
    if (!(kv.first == pivot) &&
        sameNonPivotRhs(pivot, norm_row, kv.first, kv.second)) {
      return addEquality(kv.first, pivot);
    }
  }

  constexpr size_t kMaxTableauRows = 48;
  if (linear_tableau_.size() >= kMaxTableauRows) {
    return true;
  }

  // Back-substitute new pivot into existing tableau rows to preserve RREF.
  std::vector<LinearEqRow> affected_rows;
  for (auto it = linear_tableau_.begin(); it != linear_tableau_.end();) {
    if (it->second.coeffs.count(pivot) > 0) {
      affected_rows.push_back(std::move(it->second));
      it = linear_tableau_.erase(it);
    } else {
      ++it;
    }
  }

  linear_tableau_[pivot] = std::move(norm_row);

  for (auto &arow : affected_rows) {
    if (!insertLinearEquationRow(std::move(arow))) {
      return false;
    }
  }

  return true;
}

bool PulseFormula::propagateLinearTableau() {
  if (in_tableau_propagate_ || is_contradiction_ || linear_tableau_.empty()) {
    return !is_contradiction_;
  }
  in_tableau_propagate_ = true;

  for (unsigned round = 0; round < 4; ++round) {
    size_t eq_before = pending_new_eqs_.size();
    size_t lo_before = lower_bounds_.size();
    size_t hi_before = upper_bounds_.size();
    bool bounds_tightened = false;

    // 1. Re-normalize all existing rows against latest union-find reps and
    //    discovered constant bounds (derives new constants/equalities!).
    std::vector<LinearEqRow> rows;
    rows.reserve(linear_tableau_.size());
    for (auto &kv : linear_tableau_) {
      rows.push_back(std::move(kv.second));
    }
    linear_tableau_.clear();

    for (auto &r : rows) {
      if (!insertLinearEquationRow(std::move(r))) {
        in_tableau_propagate_ = false;
        return false;
      }
    }

    // 2. Interval bound propagation across every tableau equation:
    //    a*v + sum(b_j*u_j) + c == 0 => a*v = -c - sum(b_j*u_j).
    for (const auto &kv : linear_tableau_) {
      const LinearEqRow &row = kv.second;
      for (const auto &target_kv : row.coeffs) {
        AbstractValue v = findRep(target_kv.first);
        int64_t a = target_kv.second;
        if (a == 0)
          continue;

        bool all_others_bounded = true;
        __int128 rhs_min = -static_cast<__int128>(row.constant);
        __int128 rhs_max = -static_cast<__int128>(row.constant);

        for (const auto &other_kv : row.coeffs) {
          if (other_kv.first == target_kv.first)
            continue;
          AbstractValue u = findRep(other_kv.first);
          auto u_lo = getLowerBound(u);
          auto u_hi = getUpperBound(u);
          if (!u_lo || !u_hi) {
            all_others_bounded = false;
            break;
          }
          __int128 neg_b = -static_cast<__int128>(other_kv.second);
          if (neg_b > 0) {
            rhs_min += neg_b * static_cast<__int128>(*u_lo);
            rhs_max += neg_b * static_cast<__int128>(*u_hi);
          } else {
            rhs_min += neg_b * static_cast<__int128>(*u_hi);
            rhs_max += neg_b * static_cast<__int128>(*u_lo);
          }
        }

        if (!all_others_bounded)
          continue;

        // Solve a*v in [rhs_min, rhs_max] over integers.
        __int128 v_lo, v_hi;
        auto ceilDiv = [](__int128 num, __int128 den) -> __int128 {
          // den > 0
          __int128 q = num / den;
          __int128 r = num % den;
          if (r > 0)
            q += 1;
          return q;
        };
        auto floorDiv = [](__int128 num, __int128 den) -> __int128 {
          // den > 0
          __int128 q = num / den;
          __int128 r = num % den;
          if (r < 0)
            q -= 1;
          return q;
        };

        if (a > 0) {
          v_lo = ceilDiv(rhs_min, a);
          v_hi = floorDiv(rhs_max, a);
        } else {
          v_lo = ceilDiv(-rhs_max, -a);
          v_hi = floorDiv(-rhs_min, -a);
        }

        if (v_lo > v_hi) {
          in_tableau_propagate_ = false;
          return false;
        }
        if (fitsInI64(v_lo) && fitsInI64(v_hi)) {
          auto old_lo = getLowerBound(v);
          auto old_hi = getUpperBound(v);
          int64_t new_lo = static_cast<int64_t>(v_lo);
          int64_t new_hi = static_cast<int64_t>(v_hi);
          if (!old_lo || new_lo > *old_lo || !old_hi || new_hi < *old_hi) {
            if (!addBounds(v, new_lo, new_hi)) {
              in_tableau_propagate_ = false;
              return false;
            }
            bounds_tightened = true;
          }
        }
      }
    }

    if (!bounds_tightened && pending_new_eqs_.size() == eq_before &&
        lower_bounds_.size() == lo_before &&
        upper_bounds_.size() == hi_before) {
      break;
    }
  }

  in_tableau_propagate_ = false;
  return true;
}

//===----------------------------------------------------------------------===//
// Incremental Difference-Constraint / Simplex Inequality Propagation
//===----------------------------------------------------------------------===//

bool PulseFormula::propagateLinearInequalities() {
  if (in_tableau_propagate_ || is_contradiction_ || !has_linear_inequalities_ ||
      linear_constraints_.empty() || linear_constraints_.size() > 128) {
    return !is_contradiction_;
  }
  in_tableau_propagate_ = true;

  // Represent normalized difference constraints: u - v <= c (exact __int128).
  using DiffKey = std::pair<AbstractValue, AbstractValue>;
  std::map<DiffKey, __int128> diff_upper;

  auto recordDiff = [&](AbstractValue u, AbstractValue v, __int128 c) -> bool {
    if (u == v) {
      return c >= 0; // u - u <= c requires c >= 0!
    }
    DiffKey key{u, v};
    auto it = diff_upper.find(key);
    if (it == diff_upper.end() || c < it->second) {
      diff_upper[key] = c;
    }
    return true;
  };

  // Extract difference constraints from linear_constraints_ (normalizing
  // strict integer inequalities `< c` to `<= c - 1`).
  for (const auto &lc : linear_constraints_) {
    if (lc.terms.size() != 2 || lc.kind == ConstraintKind::Equal)
      continue;
    AbstractValue v0 = findRep(lc.terms[0].var);
    AbstractValue v1 = findRep(lc.terms[1].var);
    int64_t c0 = lc.terms[0].coefficient;
    int64_t c1 = lc.terms[1].coefficient;
    __int128 rhs = lc.constant;

    // Normalize to `u - v <= bound`.
    if (c0 == 1 && c1 == -1) {
      switch (lc.kind) {
      case ConstraintKind::LessEqual:
        if (!recordDiff(v0, v1, rhs)) {
          in_tableau_propagate_ = false;
          return false;
        }
        break;
      case ConstraintKind::Less:
        if (!recordDiff(v0, v1, rhs - 1)) {
          in_tableau_propagate_ = false;
          return false;
        }
        break;
      case ConstraintKind::GreaterEqual:
        if (!recordDiff(v1, v0, -rhs)) {
          in_tableau_propagate_ = false;
          return false;
        }
        break;
      case ConstraintKind::Greater:
        if (!recordDiff(v1, v0, -rhs - 1)) {
          in_tableau_propagate_ = false;
          return false;
        }
        break;
      default:
        break;
      }
    } else if (c0 == -1 && c1 == 1) {
      switch (lc.kind) {
      case ConstraintKind::LessEqual:
        if (!recordDiff(v1, v0, rhs)) {
          in_tableau_propagate_ = false;
          return false;
        }
        break;
      case ConstraintKind::Less:
        if (!recordDiff(v1, v0, rhs - 1)) {
          in_tableau_propagate_ = false;
          return false;
        }
        break;
      case ConstraintKind::GreaterEqual:
        if (!recordDiff(v0, v1, -rhs)) {
          in_tableau_propagate_ = false;
          return false;
        }
        break;
      case ConstraintKind::Greater:
        if (!recordDiff(v0, v1, -rhs - 1)) {
          in_tableau_propagate_ = false;
          return false;
        }
        break;
      default:
        break;
      }
    }
  }

  // Bounded Floyd-Warshall / Bellman-Ford closure over difference graph:
  // detects negative cycles (`x < y ∧ y < z ∧ z < x`) AND antisymmetric
  // equalities (`x <= y ∧ y <= x => x == y`!) eagerly without Z3.
  std::set<AbstractValue> diff_nodes;
  for (const auto &kv : diff_upper) {
    diff_nodes.insert(kv.first.first);
    diff_nodes.insert(kv.first.second);
  }
  if (diff_nodes.size() <= 32) {
    bool changed = true;
    for (unsigned iter = 0; iter < 8 && changed; ++iter) {
      changed = false;
      std::vector<std::pair<DiffKey, __int128>> snapshot(diff_upper.begin(),
                                                         diff_upper.end());
      for (const auto &e1 : snapshot) {
        AbstractValue a = e1.first.first;
        AbstractValue b = e1.first.second;
        __int128 w1 = e1.second;
        for (const auto &e2 : snapshot) {
          if (!(e2.first.first == b))
            continue;
          AbstractValue c = e2.first.second;
          __int128 w = w1 + e2.second;
          if (a == c) {
            if (w < 0) {
              in_tableau_propagate_ = false;
              return false; // Negative cycle -> UNSAT!
            }
            continue;
          }
          DiffKey k{a, c};
          auto it = diff_upper.find(k);
          if (it == diff_upper.end() || w < it->second) {
            diff_upper[k] = w;
            changed = true;
          }
        }
      }
    }
  }

  // Propagate bounds and discover antisymmetric equalities (u - v <= c &&
  // v - u <= -c => u - v == c!).
  std::vector<std::pair<AbstractValue, AbstractValue>> derived_eqs;
  std::vector<LinearEqRow> derived_rows;
  for (const auto &kv : diff_upper) {
    AbstractValue u = kv.first.first;
    AbstractValue v = kv.first.second;
    __int128 c = kv.second;

    // Interval bound tightening: u <= v + c and v >= u - c.
    if (auto v_hi = getUpperBound(v)) {
      __int128 u_hi = static_cast<__int128>(*v_hi) + c;
      if (fitsInI64(u_hi)) {
        auto old_hi = getUpperBound(u);
        if (!old_hi || static_cast<int64_t>(u_hi) < *old_hi) {
          if (!addBounds(u, std::numeric_limits<int64_t>::min(),
                         static_cast<int64_t>(u_hi))) {
            in_tableau_propagate_ = false;
            return false;
          }
        }
      }
    }
    if (auto u_lo = getLowerBound(u)) {
      __int128 v_lo = static_cast<__int128>(*u_lo) - c;
      if (fitsInI64(v_lo)) {
        auto old_lo = getLowerBound(v);
        if (!old_lo || static_cast<int64_t>(v_lo) > *old_lo) {
          if (!addBounds(v, static_cast<int64_t>(v_lo),
                         std::numeric_limits<int64_t>::max())) {
            in_tableau_propagate_ = false;
            return false;
          }
        }
      }
    }

    auto rev_it = diff_upper.find({v, u});
    if (rev_it != diff_upper.end()) {
      __int128 sum = c + rev_it->second;
      if (sum < 0) {
        in_tableau_propagate_ = false;
        return false; // Contradiction!
      }
      if (sum == 0 && u < v) {
        if (c == 0) {
          derived_eqs.emplace_back(u, v);
        } else if (fitsInI64(c)) {
          LinearEqRow row;
          row.coeffs[u] = 1;
          row.coeffs[v] = -1;
          row.constant = static_cast<int64_t>(-c);
          derived_rows.push_back(std::move(row));
        }
      }
    }
  }

  for (const auto &eq : derived_eqs) {
    if (!addEquality(eq.first, eq.second)) {
      in_tableau_propagate_ = false;
      return false;
    }
  }
  for (auto &row : derived_rows) {
    if (!insertLinearEquationRow(std::move(row))) {
      in_tableau_propagate_ = false;
      return false;
    }
  }

  in_tableau_propagate_ = false;
  if ((!derived_eqs.empty() || !derived_rows.empty()) &&
      !linear_tableau_.empty()) {
    return propagateLinearTableau();
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Type / InstanceOf Predicates & Subtype Hierarchy
//===----------------------------------------------------------------------===//

void PulseFormula::registerSubtype(const std::string &derived_type,
                                   const std::string &base_type) {
  if (!derived_type.empty() && !base_type.empty() &&
      derived_type != base_type) {
    subtypeHierarchyMap()[derived_type].insert(base_type);
  }
}

void PulseFormula::clearSubtypeHierarchy() { subtypeHierarchyMap().clear(); }

std::set<std::string> PulseFormula::getSupertypes(const std::string &type_name) {
  std::set<std::string> visited;
  if (type_name.empty()) {
    return visited;
  }
  visited.insert(type_name);
  const auto &edges = subtypeHierarchyMap();
  if (edges.find(type_name) == edges.end()) {
    return visited;
  }
  std::queue<std::string> q;
  q.push(type_name);
  while (!q.empty()) {
    std::string cur = std::move(q.front());
    q.pop();
    auto it = edges.find(cur);
    if (it == edges.end())
      continue;
    for (const auto &parent : it->second) {
      if (visited.insert(parent).second) {
        q.push(parent);
      }
    }
  }
  return visited;
}

bool PulseFormula::isSubtypeOf(const std::string &derived_type,
                               const std::string &base_type) {
  if (derived_type.empty() || base_type.empty()) {
    return false;
  }
  if (derived_type == base_type) {
    return true;
  }
  const auto &edges = subtypeHierarchyMap();
  if (edges.find(derived_type) == edges.end()) {
    return false;
  }
  return getSupertypes(derived_type).count(base_type) > 0;
}

bool PulseFormula::addExactType(AbstractValue v, const std::string &type_name) {
  if (is_contradiction_ || type_name.empty()) {
    return !is_contradiction_;
  }
  AbstractValue rep = findRep(v);
  auto it = exact_types_.find(rep);
  if (it != exact_types_.end() && it->second != type_name) {
    return false;
  }
  const auto &edges = subtypeHierarchyMap();
  const bool has_hierarchy_edges = edges.find(type_name) != edges.end();
  auto nio = not_instanceof_types_.find(rep);
  if (!has_hierarchy_edges) {
    if (nio != not_instanceof_types_.end() && nio->second.count(type_name) > 0) {
      return false;
    }
    if (it != exact_types_.end()) {
      return true;
    }
    invalidateSatCache();
    exact_types_[rep] = type_name;
    return true;
  }
  std::set<std::string> supers = getSupertypes(type_name);
  if (nio != not_instanceof_types_.end()) {
    for (const auto &st : supers) {
      if (nio->second.count(st) > 0) {
        return false;
      }
    }
  }
  invalidateSatCache();
  exact_types_[rep] = type_name;
  for (const auto &st : supers) {
    if (st != type_name) {
      instanceof_types_[rep].insert(st);
    }
  }
  return true;
}

bool PulseFormula::addInstanceOf(AbstractValue v,
                                 const std::string &type_name) {
  if (is_contradiction_ || type_name.empty()) {
    return !is_contradiction_;
  }
  AbstractValue rep = findRep(v);
  auto et = exact_types_.find(rep);
  if (et != exact_types_.end() && !isSubtypeOf(et->second, type_name)) {
    return false;
  }
  std::set<std::string> supers = getSupertypes(type_name);
  auto nio = not_instanceof_types_.find(rep);
  if (nio != not_instanceof_types_.end()) {
    for (const auto &st : supers) {
      if (nio->second.count(st) > 0) {
        return false;
      }
    }
  }
  invalidateSatCache();
  for (const auto &st : supers) {
    instanceof_types_[rep].insert(st);
  }
  return true;
}

bool PulseFormula::addNotInstanceOf(AbstractValue v,
                                    const std::string &type_name) {
  if (is_contradiction_ || type_name.empty()) {
    return !is_contradiction_;
  }
  AbstractValue rep = findRep(v);
  auto et = exact_types_.find(rep);
  if (et != exact_types_.end() && isSubtypeOf(et->second, type_name)) {
    return false;
  }
  auto io = instanceof_types_.find(rep);
  if (io != instanceof_types_.end()) {
    for (const auto &t : io->second) {
      if (isSubtypeOf(t, type_name)) {
        return false;
      }
    }
  }
  invalidateSatCache();
  not_instanceof_types_[rep].insert(type_name);
  return true;
}

std::optional<std::string>
PulseFormula::getExactType(AbstractValue v) const {
  AbstractValue rep = findRepReadOnly(v);
  auto it = exact_types_.find(rep);
  return (it != exact_types_.end()) ? std::optional<std::string>(it->second)
                                    : std::nullopt;
}

bool PulseFormula::isInstanceOf(AbstractValue v,
                                const std::string &type_name) const {
  if (type_name.empty()) {
    return false;
  }
  AbstractValue rep = findRepReadOnly(v);
  auto et = exact_types_.find(rep);
  if (et != exact_types_.end() && isSubtypeOf(et->second, type_name)) {
    return true;
  }
  auto io = instanceof_types_.find(rep);
  if (io != instanceof_types_.end()) {
    for (const auto &t : io->second) {
      if (isSubtypeOf(t, type_name)) {
        return true;
      }
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// String & Regexp Theory
//===----------------------------------------------------------------------===//

bool PulseFormula::addStringConstant(AbstractValue v, const std::string &str) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue rep = findRep(v);
  auto it = string_constants_.find(rep);
  if (it != string_constants_.end() && it->second != str) {
    return false;
  }
  invalidateSatCache();
  string_constants_[rep] = str;
  auto lit = string_lengths_.find(rep);
  if (lit != string_lengths_.end()) {
    int64_t len = static_cast<int64_t>(str.size());
    if (!addBounds(lit->second, len, len)) {
      return false;
    }
  }
  for (const auto &rm : string_regex_matches_) {
    if (findRep(rm.value) == rep) {
      try {
        bool hit = std::regex_search(str, std::regex(rm.pattern));
        if (hit != rm.positive) {
          return false;
        }
      } catch (const std::regex_error &) {
      }
    }
  }
  return true;
}

std::optional<std::string>
PulseFormula::asConstantString(AbstractValue v) const {
  AbstractValue rep = findRepReadOnly(v);
  auto it = string_constants_.find(rep);
  return (it != string_constants_.end())
             ? std::optional<std::string>(it->second)
             : std::nullopt;
}

bool PulseFormula::addStringLength(AbstractValue str_v, AbstractValue len_v) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue rs = findRep(str_v);
  AbstractValue rl = findRep(len_v);
  addIntegerConstraint(rl);
  if (!addBounds(rl, 0, std::numeric_limits<int64_t>::max())) {
    return false;
  }
  auto existing = string_lengths_.find(rs);
  if (existing != string_lengths_.end()) {
    if (!addEquality(existing->second, rl)) {
      return false;
    }
  } else {
    string_lengths_[rs] = rl;
  }
  if (auto s = asConstantString(rs)) {
    int64_t len = static_cast<int64_t>(s->size());
    if (!addBounds(rl, len, len)) {
      return false;
    }
  }
  return true;
}

bool PulseFormula::addStringConcat(AbstractValue result, AbstractValue lhs,
                                   AbstractValue rhs) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue rr = findRep(result);
  AbstractValue r1 = findRep(lhs);
  AbstractValue r2 = findRep(rhs);
  auto s1 = asConstantString(r1);
  auto s2 = asConstantString(r2);
  if (s1 && s2) {
    if (!addStringConstant(rr, *s1 + *s2)) {
      return false;
    }
  } else if (s1 && s1->empty()) {
    if (!addEquality(rr, r2))
      return false;
  } else if (s2 && s2->empty()) {
    if (!addEquality(rr, r1))
      return false;
  }
  // Link lengths via linear arithmetic if all three have tracked length AVs.
  auto lr = string_lengths_.find(rr);
  auto l1 = string_lengths_.find(r1);
  auto l2 = string_lengths_.find(r2);
  if (lr != string_lengths_.end() && l1 != string_lengths_.end() &&
      l2 != string_lengths_.end()) {
    if (!addArithmeticOperation(lr->second, l1->second, l2->second, "+")) {
      return false;
    }
  }
  string_concats_.push_back(StringConcat{rr, r1, r2});
  return true;
}

bool PulseFormula::addStringRegexMatch(AbstractValue v,
                                       const std::string &pattern,
                                       bool positive) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue rep = findRep(v);
  for (const auto &existing : string_regex_matches_) {
    if (findRep(existing.value) == rep && existing.pattern == pattern &&
        existing.positive != positive) {
      return false;
    }
  }
  if (auto s = asConstantString(rep)) {
    try {
      bool hit = std::regex_search(*s, std::regex(pattern));
      if (hit != positive) {
        return false;
      }
    } catch (const std::regex_error &) {
    }
  }
  invalidateSatCache();
  string_regex_matches_.push_back(StringRegexMatch{rep, pattern, positive});
  return true;
}

//===----------------------------------------------------------------------===//
// Floating-Point Constraints & Bit-Widths
//===----------------------------------------------------------------------===//

bool PulseFormula::addFloatConstant(AbstractValue v, double val) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue rep = findRep(v);
  auto it = float_constants_.find(rep);
  if (it != float_constants_.end() && it->second != val) {
    return false;
  }
  invalidateSatCache();
  float_constants_[rep] = val;
  return true;
}

std::optional<double> PulseFormula::asConstantFloat(AbstractValue v) const {
  AbstractValue rep = findRepReadOnly(v);
  auto it = float_constants_.find(rep);
  return (it != float_constants_.end()) ? std::optional<double>(it->second)
                                        : std::nullopt;
}

bool PulseFormula::addFloatOperation(AbstractValue result, AbstractValue op1,
                                     AbstractValue op2, const std::string &op) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue r_res = findRep(result);
  AbstractValue r1 = findRep(op1);
  AbstractValue r2 = findRep(op2);

  auto c1 = asConstantFloat(r1);
  auto c2 = asConstantFloat(r2);
  if (c1 && c2) {
    if (op == "/" && *c2 == 0.0) {
      return false;
    }
    double folded = 0.0;
    if (op == "+")
      folded = *c1 + *c2;
    else if (op == "-")
      folded = *c1 - *c2;
    else if (op == "*")
      folded = *c1 * *c2;
    else if (op == "/")
      folded = *c1 / *c2;
    else
      return true;
    if (!addFloatConstant(r_res, folded)) {
      return false;
    }
  }
  constexpr size_t kMaxFloatOps = 64;
  if (float_ops_.size() < kMaxFloatOps) {
    invalidateSatOnly();
    float_ops_.push_back(FloatOp{r_res, r1, r2, op});
  }
  return in_tableau_propagate_ ? true : isConsistent();
}

bool PulseFormula::addFloatComparison(AbstractValue op1, AbstractValue op2,
                                      const std::string &pred) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue r1 = findRep(op1);
  AbstractValue r2 = findRep(op2);
  auto c1 = asConstantFloat(r1);
  auto c2 = asConstantFloat(r2);
  if (c1 && c2) {
    return evalFloatCmp(pred, *c1, *c2);
  }
  if (r1 == r2 && (pred == "!=" || pred == "<" || pred == ">")) {
    return false;
  }
  invalidateSatCache();
  float_cmps_.push_back(FloatCmp{r1, r2, pred});
  return isConsistent();
}

void PulseFormula::setBitWidth(AbstractValue v, unsigned bits) {
  if (is_contradiction_ || bits == 0)
    return;
  bit_widths_[findRep(v)] = bits;
}

std::optional<unsigned> PulseFormula::getBitWidth(AbstractValue v) const {
  AbstractValue rep = findRepReadOnly(v);
  auto it = bit_widths_.find(rep);
  return (it != bit_widths_.end()) ? std::optional<unsigned>(it->second)
                                   : std::nullopt;
}

//===----------------------------------------------------------------------===//
// Disjunctive Equalities from PulseJoin
//===----------------------------------------------------------------------===//

bool PulseFormula::addDisjunctiveEquality(AbstractValue target,
                                          AbstractValue opt1,
                                          AbstractValue opt2,
                                          std::optional<int64_t> c1,
                                          std::optional<int64_t> c2) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue rt = findRep(target);
  if (c1 && c2) {
    if (*c1 == *c2) {
      return addBounds(rt, *c1, *c1);
    }
    int64_t lo = std::min(*c1, *c2);
    int64_t hi = std::max(*c1, *c2);
    if (!addBounds(rt, lo, hi)) {
      return false;
    }
    if (*c1 != 0 && *c2 != 0) {
      if (!addNonNull(rt)) {
        return false;
      }
    }
    constexpr size_t kMaxDisjEqs = 64;
    if (disjunctive_eqs_.size() < kMaxDisjEqs) {
      invalidateSatOnly();
      DisjunctiveEq deq;
      deq.target = rt;
      deq.const_options = {*c1, *c2};
      disjunctive_eqs_.push_back(std::move(deq));
    }
    return in_tableau_propagate_ ? true : isConsistent();
  }

  AbstractValue ro1 = findRep(opt1);
  AbstractValue ro2 = findRep(opt2);
  if (ro1 == ro2) {
    return addEquality(rt, ro1);
  }
  constexpr size_t kMaxDisjEqs = 64;
  if (disjunctive_eqs_.size() < kMaxDisjEqs) {
    invalidateSatOnly();
    DisjunctiveEq deq;
    deq.target = rt;
    deq.var_options = {ro1, ro2};
    disjunctive_eqs_.push_back(std::move(deq));
  }
  return true;
}

} // namespace pulse
