#pragma once

#include "Verification/Mosaic/Z3Utils.h"

#include <map>
#include <optional>
#include <set>

#include <z3++.h>

namespace lotus::mosaic {
static constexpr unsigned MAX_MTH_BV_SIZE = 63;

// Centralized output sinks (default to std::cout/std::cerr)
inline std::ostream *g_out = &std::cout;
inline std::ostream *g_err = &std::cerr;

inline void set_output_stream(std::ostream &os) { g_out = &os; }
inline void set_error_stream(std::ostream &os) { g_err = &os; }

inline std::ostream &OUT() { return *g_out; }
inline std::ostream &ERR() { return *g_err; }

#define MOSAIC_NOT_IMPLEMENTED()                                               \
  do {                                                                         \
    OUT() << "Not implemented: " << __FILE__ << ":" << __LINE__ << std::endl;  \
    assert(false && "Not implemented");                                        \
    exit(1);                                                                   \
  } while (0)

#define MOSAIC_UNREACHABLE()                                                   \
  do {                                                                         \
    OUT() << "Unreachable code reached: " << __FILE__ << ":" << __LINE__       \
          << std::endl;                                                        \
    assert(false && "Unreachable code reached");                               \
    exit(1);                                                                   \
  } while (0)

// Add assert(false) with message macro
#define MOSAIC_ASSERT_FALSE(msg)                                               \
  do {                                                                         \
    OUT() << "Assertion failed: " << msg << " at " << __FILE__ << ":"          \
          << __LINE__ << std::endl;                                            \
    assert(false && msg);                                                      \
    exit(1);                                                                   \
  } while (0)

// DEBUG related stuff
inline bool mtfp_debug = false;
inline void set_mtfp_debug(bool is_debug) { mtfp_debug = is_debug; }
#define MOSAIC_DEBUG_MSG(cmd)                                                  \
  do {                                                                         \
    if (mtfp_debug) {                                                          \
      OUT() << "-------------------------------------------------------"       \
            << std::endl;                                                      \
      cmd;                                                                     \
    }                                                                          \
  } while (0)

struct AstHash {
  std::size_t operator()(Z3_ast a) const noexcept {
    return reinterpret_cast<std::size_t>(a);
  }
};
struct AstEq {
  bool operator()(Z3_ast a, Z3_ast b) const noexcept { return a == b; }
};

struct compare_func_decl {
  bool operator()(const z3::func_decl &lhs, const z3::func_decl &rhs) const {
    return lhs.id() < rhs.id();
  }
};

struct compare_expr {
  bool operator()(const z3::expr &lhs, const z3::expr &rhs) const {
    return lhs.hash() < rhs.hash();
  }
};

using VarMap = std::map<z3::expr, z3::expr, compare_expr>;
inline std::string dump(const VarMap &var_map) {
  std::string result = "{\n";
  for (const auto &pair : var_map) {
    result +=
        "  " + pair.first.to_string() + " -> " + pair.second.to_string() + "\n";
  }
  result += "}";
  return result;
}
using VarSet = std::set<z3::expr, compare_expr>;
using VarIndexMap = std::map<z3::expr, int, compare_expr>;
using VarLemmaMap = std::map<z3::expr, z3::expr, compare_expr>;
class PredicateMap {
  using Map = std::map<z3::func_decl, z3::expr, compare_func_decl>;

  std::map<z3::func_decl, z3::expr_vector, compare_func_decl> dstVarMap;
  Map Theta;

public:
  // The interface constraint is p1 -> p2
  // The key is the destination predicate p2 because of the
  // assumption that each predicate has at most one predecessor
  // Return true if insertion happened, false if p2 -> p1 already exists.
  bool insert(z3::expr &p1, z3::expr &p2) {
    auto it = Theta.find(p2.decl());
    // If p2 -> p1 is already in the map, do nothing
    if (it != Theta.end() && z3::eq(it->second.decl(), p1.decl()))
      return false;
    assert(it == Theta.end() &&
           "PredicateMap: first predicate is already mapped");
    Theta.emplace(p2.decl(), p1);
    dstVarMap.emplace(p2.decl(), utils::get_args(p2));
    return true;
  }

  std::optional<z3::expr> find_pred(const z3::func_decl &dst) const {
    // Given a destination predicate, return the source predicate
    auto it = Theta.find(dst);
    if (it != Theta.end()) {
      return it->second; // Return the source predicate
    }
    return std::nullopt; // Not found
  }

  z3::expr_vector get_dst_vars(const z3::func_decl &dst) const {
    // Get the destination variables for a given source predicate
    auto it = dstVarMap.find(dst);
    assert(it != dstVarMap.end() && "PredicateMap: destination variables not "
                                    "found for the given predicate");
    return it->second; // Return the destination variables
  }
};

struct CHC {
  // TODO: Get rid off vars, create fresh ones.
  z3::expr_vector const vars;
  z3::expr body_preds;
  z3::expr body_formula;
  z3::expr head;

  CHC(z3::expr_vector const &v, z3::expr bp, z3::expr bf, z3::expr h)
      : vars(v), body_preds(bp), body_formula(bf), head(h) {}

  z3::expr get_rule_expr() const {
    assert(!head.is_true() ||
           !head.is_false() &&
               "Head of normal CHC rule cannot be a boolean expression");
    return z3::forall(vars, z3::implies(body_preds && body_formula, head));
  }

  z3::expr get_query_expr() const {
    assert(head.is_false() && "Head of query CHC must be false");
    return z3::exists(vars, body_preds && body_formula);
  }

  z3::expr get_body_expr() const { return body_preds && body_formula; }
};

struct MTHSolver {
  z3::fixedpoint fp_solver;
  z3::expr query;
  unsigned bv_size;
  bool is_bv;

  static constexpr unsigned UNDETERMINED_BV_SIZE = 0;

  MTHSolver(z3::context &ctx, bool is_bv,
            unsigned bv_size = UNDETERMINED_BV_SIZE)
      : fp_solver(ctx), query(ctx), bv_size(bv_size), is_bv(is_bv) {}
  z3::expr_vector get_all_clauses() const;
  bool is_bv_solver() const { return is_bv; }
  unsigned get_bv_size() const;
};

class MTHFixedpointSet {
private:
  z3::context &ctx;

  // Tracks the signedness of the entire set
  std::optional<bool> global_is_signed;
  std::optional<MTHSolver> bv_solver;
  std::map<unsigned, MTHSolver> iauf_solvers;

  // Map rule unique ID -> vector of body predicate exprs
  std::map<unsigned, z3::expr_vector> rule_body_preds_map;
  // Map rule unique ID -> head predicate expr
  std::map<unsigned, z3::expr> rule_head_pred_map;
  // Map rule unique ID -> solver it belongs to
  std::map<unsigned, MTHSolver *> rule_solver_map;
  // Map predicate expr ID -> solver it belongs to
  std::map<z3::func_decl, MTHSolver *, compare_func_decl> pred_solver_map;

  const std::string rule_name = "__mth_rule__";
  unsigned rule_count = 0;

public:
  MTHFixedpointSet(z3::context &ctx) : ctx(ctx) {}

  // Checks if the incoming signedness is compatible with the set.
  // If the set is empty, it sets the signedness.
  // Returns true if:
  // 1. The set is empty (sign not yet determined) -> Trivial true
  // 2. The set is not empty and the signs match.
  // Returns false only if there is a conflict.
  bool check_and_set_signedness(bool incoming_sign);

  std::optional<bool> get_global_signedness() const { return global_is_signed; }

  bool hasBVSolver() const;
  MTHSolver &getOrInitBVSolver();
  MTHSolver &getBVSolver();

  bool hasIAUFSolver(unsigned bv_size) const;
  MTHSolver &getOrInitIAUFSolver(unsigned bv_size);
  MTHSolver &getIAUFSolver(unsigned bv_size);
  std::map<unsigned, MTHSolver> &getIAUFSolvers() { return iauf_solvers; }

  z3::symbol get_fresh_rule_name();

  void populate_predicate_maps_for_clause(const z3::expr clause_expr,
                                          MTHSolver *solver, bool is_query);
  std::optional<z3::expr_vector>
  get_clause_body_preds(const z3::expr &clause) const;
  std::optional<z3::expr> get_clause_head_pred(const z3::expr &clause) const;

  void map_clause_to_solver(const z3::expr &clause, MTHSolver *solver);
  MTHSolver *get_clause_solver(const z3::expr &clause) const;
  MTHSolver *get_predicate_solver(const z3::func_decl &pred) const;

  friend std::ostream &operator<<(std::ostream &os,
                                  const MTHFixedpointSet &mth_fp_set);
};

enum class Signedness { UNKNOWN, SIGNED, UNSIGNED, CONFLICT };
static constexpr unsigned UNDETERM_BV_SIZE =
    std::numeric_limits<unsigned>::max();
struct ClauseAnalysisResult {
  Signedness is_signed;
  unsigned bv_size; // UNDETERM_BV_SIZE if not uniquely determined.
  bool has_bit_manipulating_apps;
  // Variables that do not occur in any predicate application
  // in the body of the clause.
  VarSet in_pred_body_vars;
  VarSet all_vars;
  // This is map is necessary as we have no other way identifying
  // variables as their API isn't exposed in Z3 C++ API.
  VarIndexMap var_index_map;

  ClauseAnalysisResult(z3::context &ctx)
      : is_signed(Signedness::UNKNOWN), bv_size(UNDETERM_BV_SIZE),
        has_bit_manipulating_apps(false), in_pred_body_vars(), all_vars() {}

  bool is_bv_size_determined() const { return bv_size != UNDETERM_BV_SIZE; }

  bool is_signedness_determined() const {
    return is_signed != Signedness::UNKNOWN &&
           is_signed != Signedness::CONFLICT;
  }

  bool has_conflicting_signedness() const {
    return is_signed == Signedness::CONFLICT;
  }

  bool get_is_signed() const {
    assert(is_signedness_determined() && "Signedness is not determined");
    if (is_signed == Signedness::SIGNED)
      return true;

    return false;
  }

  // Declare operator<< for easy printing
  friend std::ostream &operator<<(std::ostream &os,
                                  const ClauseAnalysisResult &result);
};

/// @brief Return an expr_vector containing all the variables of the CHC clause.
/// A CHC clause is of the form:
/// forall x1 ... xn. (body => head)
/// @param clause The CHC clause to extract variables from.
/// @return An expr_vector containing all the variables of the CHC clause.
z3::expr_vector get_clause_vars(z3::expr const &clause);

/// @brief Get the head of the CHC clause. If the clause is a query,
/// it returns an empty expression.
/// @param clause The CHC clause to extract the head from.
/// @param is_query Whether the clause is a query or a rule.
/// @return The head of the CHC clause.
z3::expr get_clause_head(z3::expr const &clause, bool is_query);

/// @brief Get the body of the CHC clause.
/// @param clause The CHC clause to extract the body from.
/// @param is_query Whether the clause is a query or a rule.
/// @return The body of the CHC clause.
z3::expr get_clause_body(z3::expr const &clause, bool is_query);

/// @brief Analyze the given CHC clause and return the result. We try to infer:
/// - The signedness of the bit-vector operations in the clause.
/// - The bit-vector size used in the clause.
/// - Whether the clause contains bit-manipulating operations.
/// - The free variables in the body of the clause.
/// @param ctx The Z3 context.
/// @param clause The CHC clause to analyze.
/// @return The result of the clause analysis in a ClauseAnalysisResult struct.
ClauseAnalysisResult analyze_clause(z3::context &ctx, z3::expr const &clause);

/// @brief Get the default parameters for the MTH fixedpoint solvers.
/// @param ctx The Z3 context.
/// @return The default parameters for the MTH fixedpoint solvers.
z3::params get_default_mth_fp_params(z3::context &ctx);

/// @brief Evaluate the given clause with the provided variable substitutions.
/// If no substitutions are provided, the variable is replaced with a new
/// constant.
/// @param clause The clause to evaluate.
/// @param unknown_vars_subs The substitutions for unknown variables.
/// @return The evaluated clause.
z3::expr evaluate_clause_vars(const z3::expr &clause,
                              VarMap &unknown_vars_subs);
} // namespace lotus::mosaic
