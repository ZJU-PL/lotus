

#pragma once
#include "Mosaic/MosaicUtils.h"
#include "Mosaic/Z3Utils.h"

#include <unordered_set>

#include <z3++.h>

namespace lotus::mosaic {
class Int2BvOverflowAnalyzer {
  z3::context &m_ctx;
  bool m_exists_const_out_of_bounds;
  z3::expr_vector m_var_in_bound_lemmas;
  z3::expr_vector m_func_out_of_bound_lemmas;

  using VarCache = std::unordered_set<Z3_ast, AstHash, AstEq>;
  VarCache m_handled_vars;

  z3::expr create_expr_in_bound_lemma(const z3::expr &e, unsigned bv_size,
                                      bool is_signed) const;
  bool is_const_in_bounds(const z3::expr &e, unsigned bv_size,
                          bool is_signed) const;
  void populate_bound_lemmas(const z3::expr &e, unsigned bv_size_vars,
                             unsigned bv_size_funcs_consts, bool is_signed_vars,
                             bool is_signed_funcs_consts);

public:
  Int2BvOverflowAnalyzer(z3::context &ctx);
  void reset();
  bool overflows(const z3::expr &e, unsigned bv_size_vars,
                 unsigned bv_size_funcs_consts, bool is_signed_vars,
                 bool is_signed_funcs_consts);
};
} // namespace lotus::mosaic