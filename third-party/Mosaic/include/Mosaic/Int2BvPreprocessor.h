//------------------------------------------------------------------------------
// Int2BvPreprocessor.h
// A header file for the Int2BvPreprocessor class, which preprocesses
// integer expressions to handle overflow and underflow
//------------------------------------------------------------------------------

#pragma once
#include "Mosaic/InstCombiner.h"
#include "Mosaic/MosaicUtils.h"

#include <unordered_set>

#include <z3++.h>

namespace lotus::mosaic {
class Int2BvPreprocessor {
private:
  z3::context &m_ctx;
  unsigned m_bv_size;
  bool m_is_signed;

  using ExprVectorMatrix = std::vector<std::vector<z3::expr_vector>>;
  using LiteralMatrix = std::vector<std::vector<z3::expr>>;
  using boolMatrix = std::vector<std::vector<bool>>;
  using VarCache = std::unordered_set<Z3_ast, AstHash, AstEq>;

  // Vars cache to not handle the same variable multiple times
  VarCache m_handled_vars;

  // Data structures to store info to build SATOutOfBounds and UNSATOutOfBounds
  // expressions
  z3::expr_vector m_vars_bound_lemmas;
  boolMatrix m_const_out_of_bounds;
  ExprVectorMatrix m_func_app_out_of_bounds;
  LiteralMatrix m_literals;

  int get_num_of_conjuncts() const;
  int get_num_of_disjuncts(int conjunct) const;

  bool is_const_variable(const z3::expr &e) const;

  z3::expr create_bounds_expr(const z3::expr &term) const;
  bool is_const_in_bounds(const z3::expr &const_e) const;
  z3::expr create_term_out_of_bounds_expr(const z3::expr &e) const;

  void populate_data_structures(const z3::expr &e);
  void handle_term(const z3::expr &term,
                   z3::expr_vector &func_app_out_of_bounds) const;
  void analyze_literal(const z3::expr &literal, bool &const_out_of_bounds,
                       z3::expr_vector &func_app_out_of_bounds);

  /// This function recursively checks e and
  /// returns true if everything is fine, i.e.,
  /// 1. @param e has no function applications
  /// 2. All constants in @param e are within bounds
  bool all_is_well(const z3::expr &e) const;

  // The below functions assume the data structures are already populated
  z3::expr create_SAT_out_of_bounds_expr(const z3::expr &e) const;
  z3::expr create_UNSAT_out_of_bounds_expr(const z3::expr &e) const;
  z3::expr create_psi_expr(const z3::expr &e) const;

  /// Creates a new CNF expr from @param e by dropping out of bounds literals
  /// marked in m_const_out_of_bounds, where m_const_out_of_bounds[i][j] maps to
  /// the j-th literal of the i-th conjunct.
  /// Returns a fresh expr in e.ctx(); @param e is unchanged.
  /// If a clause becomes empty after drops, the result is false.
  z3::expr drop_out_of_bounds_literals(const z3::expr &e) const;

public:
  explicit Int2BvPreprocessor(z3::context &c, unsigned bv_size, bool is_signed);
  void reset();

  // Exposing main functionalities for testing purposes.
  // Intended use is only for testing.
  z3::expr create_SAT_out_of_bounds(const z3::expr &e);
  z3::expr create_UNSAT_out_of_bounds(const z3::expr &e);

  z3::expr preprocess(const z3::expr &e);
};
} // namespace lotus::mosaic