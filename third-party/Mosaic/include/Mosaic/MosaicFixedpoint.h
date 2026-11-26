//------------------------------------------------------------------------------
// multi_theory_fixedpoint.h
// Multi-Theory Fixedpoint Engine Header
// Based on the paper:
//   Omer Rappoport, Orna Grumberg, and Yakir Vizel.
//   "Bit-Precise CHC Satisfiability Using Theory-Modular Reasoning."
//   ATVA 2026.
//------------------------------------------------------------------------------
#pragma once
#include "Mosaic/Bv2IntTranslator.h"
#include "Mosaic/Int2BvTranslator.h"
#include "Mosaic/MosaicUtils.h"
#include "Mosaic/Z3Utils.h"

#include <stack>

#include <z3++.h>

namespace lotus::mosaic {
class MosaicFixedpoint {
private:
  z3::context &m_ctx;
  // Whether to treat bit-vectors as signed or unsigned
  std::optional<bool> m_is_signed;
  // Whether to simplify the translations (default: true)
  bool m_simplify;
  // Whether to preprocess integer translator formulas (default: false)
  bool m_int2bv_preprocess;

  // The set of fixedpoint solvers
  MTHFixedpointSet m_mth_fp_set;
  // The parameters for all the engines
  bool m_params_set;
  z3::params m_params;

  z3::expr_vector m_original_clauses;
  std::map<z3::expr, ClauseAnalysisResult, compare_expr> m_clause_analysis_map;
  PredicateMap m_interface_constraint_map;

  using PredicateToExprMap =
      std::map<z3::func_decl, z3::expr, compare_func_decl>;
  using CHCFactConfig = std::pair<CHC, z3::symbol>;
  using PredicateToCHCConfigMap =
      std::map<z3::func_decl, CHCFactConfig, compare_func_decl>;
  PredicateToExprMap m_interface_src_strengthening_map;
  PredicateToCHCConfigMap m_interface_dst_fact_map;
  std::map<z3::func_decl, z3::expr_vector, compare_func_decl>
      m_interface_dst_vars;
  PredicateToExprMap m_interface_dst_orig_head_to_clause_map;

  std::string kAdded_fact_name = "__added_fact__";
  unsigned added_fact_counter = 0;
  std::string get_fresh_added_fact_name();

  /// @brief A method that finds the leaf predicate of a refutation.
  /// @param refutation The refutation expression to analyze.
  z3::expr get_refutation_leaf_pred(z3::expr const &refutation) const;

  /// @brief A method that creates a phi constraint based on the values the leaf
  /// predicate is assigned
  /// @param q The leaf predicate of the refutation.
  /// @param vars The vector of variables of the leaf predicate.
  /// @return A phi constraint of the form `var_1 == q.arg(0) && var_2 ==
  /// q.arg(1) && ...`
  z3::expr get_refutation_leaf_phi(z3::expr const &q,
                                   z3::expr_vector const &vars) const;

  /// @brief A function that return a conjunction of bit-vector bound
  /// expressions of the form `0 <= var < 2^bv_size` for each variable in
  /// `vars`.
  /// @param vars The vector of bit-vector variables for which to create the
  /// bound expressions.
  /// @param bv_size The size of the bit-vectors.
  z3::expr get_bv_expr_bounds(z3::expr_vector const &vars,
                              unsigned bv_size) const;

  /// @brief Checks the signedness consistency of the clause analysis result.
  /// @param clause_analysis The clause analysis result to check.
  void
  check_signedness_consistency(ClauseAnalysisResult const &clause_analysis);

  /// @brief Populates the fixedpoint engines before executing queries.
  /// This includes going over the original clauses, their analysis results,
  /// and adding them to the appropriate fixedpoint engine after translation
  /// if necessary.
  /// @param query The query expression.
  /// @param query_analysis The analysis result of the query.
  /// @return The appropriate query expression. Could be the original query or
  /// a translated version depending on the theories involved.
  z3::expr
  populate_MTH_fixedpoint_engines(const z3::expr &query,
                                  ClauseAnalysisResult const &query_analysis);

  /// @brief Adds a behind the scenes fact corresponding to the predicate given
  /// by p_expr which is the destination of an interface constraint.
  /// @param src_expr The key expr of the source predicate.
  /// @param dst_expr The fact's head (the destination predicate of the
  /// interface constraint).
  /// @param dst_fp The fixedpoint engine of the destination predicate.
  void add_predicate_fact(z3::expr const &src_expr, z3::expr const &dst_expr,
                          MTHSolver *dst_fp);

  /// @brief Adds an interface constraint (mapping) between two predicates
  /// in different theories.
  /// @param p1_expr The source predicate.
  /// @param p2_expr The target predicate.
  /// @param fp2 The solver of the target to which we add a fact.
  void add_interface_constraint(z3::expr p1_expr, z3::expr p2_expr,
                                MTHSolver *fp2);

  /// @brief Generates all needed interface constraints between all
  /// populated fixedpoint engines.
  void generate_interface_constraints();

  /// @brief Sets the parameters for all fixedpoint engines.
  /// If m_params_set is true, uses m_params. Otherwise, uses default
  /// parameters.
  void set_params_for_all_engines();

public:
  //--------------------------------------------------------------------------
  // Construction / destruction
  //--------------------------------------------------------------------------
  explicit MosaicFixedpoint(z3::context &ctx, bool force_preprocess = false,
                            bool simplify = true);

  /// @brief Initializes the multi-theory fixedpoint engine from
  /// an existing BV fixedpoint engine.
  /// @param fp The existing BV fixedpoint engine.
  void from_solver(z3::fixedpoint &fp);

  //--------------------------------------------------------------------------
  // Forwarding of fixepoint most common calles
  //--------------------------------------------------------------------------

  /// @brief Add a rule to the appropriate fixedpoint engine.
  /// @param rule The rule to add.
  /// @param name The name of the rule.
  void add_rule(z3::expr &rule, z3::symbol const &name);

  /// @brief Register a relation (predicate) in the appropriate fixedpoint
  /// engine.
  /// @param p The predicate to register.
  void register_relation(z3::func_decl &p);

  /// @brief Set parameters for both all fixedpoint engines.
  /// @param p The parameters to set.
  void set(z3::params const &p);

  /// @brief The query method for the multi-theory fixedpoint engine.
  /// @param query The query expression.
  /// @return The result of the query.
  z3::check_result query(z3::expr &query);
};
} // namespace lotus::mosaic