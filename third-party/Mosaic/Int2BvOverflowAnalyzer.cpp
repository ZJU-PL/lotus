#include "Mosaic/Int2BvOverflowAnalyzer.h"

#define LDBG(x) MOSAIC_DEBUG_MSG(OUT() << "[OverflowAnalyzer] " << x)

namespace lotus::mosaic {

static bool is_const_variable(const z3::expr &e) {
  // Check if the expression is a variable (0-arity application)
  return e.is_const() && !e.is_numeral() && !e.is_bool();
}

static bool is_func_app(Z3_decl_kind k) {
  return (k == Z3_OP_ADD || k == Z3_OP_SUB || k == Z3_OP_MUL ||
          k == Z3_OP_UMINUS || k == Z3_OP_DIV || k == Z3_OP_IDIV);
}

Int2BvOverflowAnalyzer::Int2BvOverflowAnalyzer(z3::context &ctx)
    : m_ctx(ctx), m_exists_const_out_of_bounds(false),
      m_var_in_bound_lemmas(ctx), m_func_out_of_bound_lemmas(ctx) {}

void Int2BvOverflowAnalyzer::reset() {
  m_exists_const_out_of_bounds = false;
  m_var_in_bound_lemmas = z3::expr_vector(m_ctx);
  m_func_out_of_bound_lemmas = z3::expr_vector(m_ctx);
  m_handled_vars.clear();
}

z3::expr Int2BvOverflowAnalyzer::create_expr_in_bound_lemma(
    const z3::expr &e, unsigned bv_size, bool is_signed) const {
  if (is_signed) {
    int64_t N = (int64_t)1 << (bv_size - 1);
    return (e >= m_ctx.int_val(-N)) && (e <= m_ctx.int_val(N - 1));
  }

  int64_t N = (uint64_t)1 << bv_size;
  return (e >= m_ctx.int_val(0)) && (e < m_ctx.int_val(N));
}

bool Int2BvOverflowAnalyzer::is_const_in_bounds(const z3::expr &e,
                                                unsigned bv_size,
                                                bool is_signed) const {
  assert(e.is_numeral() && "Expected a constant expression");
  if (is_signed) {
    int64_t lower_bound = utils::get_signed_bv_lower_bound(bv_size);
    int64_t upper_bound = utils::get_signed_bv_upper_bound(bv_size);
    return e.get_numeral_int64() >= lower_bound &&
           e.get_numeral_int64() <= upper_bound;
  }

  uint64_t lower_bound = utils::get_unsigned_bv_lower_bound(bv_size);
  uint64_t upper_bound = utils::get_unsigned_bv_upper_bound(bv_size);
  return static_cast<uint64_t>(e.get_numeral_int64()) >= lower_bound &&
         static_cast<uint64_t>(e.get_numeral_int64()) < upper_bound;
}

void Int2BvOverflowAnalyzer::populate_bound_lemmas(
    const z3::expr &e, unsigned bv_size_vars, unsigned bv_size_funcs_consts,
    bool is_signed_vars, bool is_signed_funcs_consts) {
  if (m_exists_const_out_of_bounds) {
    // If we already found a constant out of bounds, no need to continue
    // checking
    return;
  }
  if (e.is_true() || e.is_false()) {
    return;
  } else if (e.is_numeral()) {
    bool in_bounds =
        is_const_in_bounds(e, bv_size_funcs_consts, is_signed_funcs_consts);
    m_exists_const_out_of_bounds |= !in_bounds;
  } else if (is_const_variable(e) &&
             m_handled_vars.find((Z3_ast)e) == m_handled_vars.end()) {
    // Add the variable to the cache if not already handled
    m_handled_vars.insert((Z3_ast)e);
    // Add the lemma: var >= -2^(n-1) && var < 2^(n-1)
    z3::expr var_bound_expr =
        create_expr_in_bound_lemma(e, bv_size_vars, is_signed_vars);
    m_var_in_bound_lemmas.push_back(var_bound_expr);
  } else {
    if (is_func_app(e.decl().decl_kind())) {
      // Add the lemma: func(arg1, arg2, ...) < -2^(n-1) || func(arg1, arg2,
      // ...) >= 2^(n-1)
      z3::expr func_bound_expr = create_expr_in_bound_lemma(
          e, bv_size_funcs_consts, is_signed_funcs_consts);
      m_func_out_of_bound_lemmas.push_back(!func_bound_expr);
    }
  }

  for (unsigned i = 0; i < e.num_args(); ++i) {
    populate_bound_lemmas(e.arg(i), bv_size_vars, bv_size_funcs_consts,
                          is_signed_vars, is_signed_funcs_consts);
  }
}

bool Int2BvOverflowAnalyzer::overflows(const z3::expr &e, unsigned bv_size_vars,
                                       unsigned bv_size_funcs_consts,
                                       bool is_signed_vars,
                                       bool is_signed_funcs_consts) {
  assert(!(is_signed_vars && !is_signed_funcs_consts) &&
         "If variables are signed, expressions must be treated as signed");
  populate_bound_lemmas(e, bv_size_vars, bv_size_funcs_consts, is_signed_vars,
                        is_signed_funcs_consts);
  if (m_exists_const_out_of_bounds) {
    LDBG("Constant out of bounds detected" << std::endl);
    return true;
  }

  // Create the overflow formula and check if it's satisfiable or not
  z3::expr formula = z3::mk_and(m_var_in_bound_lemmas) &&
                     z3::mk_or(m_func_out_of_bound_lemmas);
  z3::solver s(m_ctx);
  s.add(formula);
  LDBG("Overflow formula: " << formula << "\n");
  if (s.check() == z3::sat) {
    return true;
  }

  assert(s.check() != z3::unknown &&
         "Solver returned unknown when checking for overflow");
  return false;
}

} // namespace lotus::mosaic