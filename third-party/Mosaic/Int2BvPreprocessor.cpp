#include "Mosaic/Int2BvPreprocessor.h"

#define PREPROCESSOR_DEBUG 1
#if PREPROCESSOR_DEBUG
#define LDBG(x) MOSAIC_DEBUG_MSG(OUT() << "[PREPROCESSOR] " << x)
#else
#define LDBG(x)                                                                \
  do {                                                                         \
  } while (0)
#endif

namespace lotus::mosaic {
static z3::expr better_simplify(const z3::expr &e) {
  z3::context &c = e.ctx();
  z3::tactic T = z3::tactic(c, "simplify") &
                 z3::tactic(c, "ctx-solver-simplify") &
                 z3::tactic(c, "simplify");

  z3::goal g(c);
  g.add(e);
  z3::apply_result r = T(g);
  return r[0].as_expr();
}

static bool is_signed_relevant_func_app(Z3_decl_kind k) {
  return (k == Z3_OP_ADD || k == Z3_OP_SUB || k == Z3_OP_MUL ||
          k == Z3_OP_UMINUS || k == Z3_OP_DIV || k == Z3_OP_IDIV);
}

static bool is_unsigned_relevant_func_app(Z3_decl_kind k) {
  return (k == Z3_OP_ADD || k == Z3_OP_SUB || k == Z3_OP_MUL ||
          k == Z3_OP_UMINUS);
}

/// @brief Creates disjunction of all models of the given formula.
/// @param e The original formula.
/// @return A disjunction of all models of the formula.
/// For example: if e is (x >= 3) && (x < 5) && (y = 2), then the result is
/// (x = 3 && y = 2) || (x = 4 && y = 2)
static z3::expr create_formula_from_models(const z3::expr &e) {
  z3::context &c = e.ctx();
  z3::solver s(c);
  s.add(e);

  z3::expr_vector models(c);
  while (s.check() == z3::sat) {
    z3::model m = s.get_model();
    z3::expr_vector model_conjuncts(c);

    for (unsigned i = 0; i < m.size(); ++i) {
      z3::func_decl v = m[i];
      if (v.arity() == 0) { // Only consider constants (variables)
        z3::expr v_val = m.get_const_interp(v);
        model_conjuncts.push_back(v() == v_val);
      }
    }

    z3::expr model_expr = model_conjuncts.size() == 1
                              ? model_conjuncts[0]
                              : z3::mk_and(model_conjuncts);
    models.push_back(model_expr);

    // Add a blocking clause to prevent the same model from being found again
    s.add(!model_expr);
  }

  if (models.empty()) {
    return c.bool_val(false); // No models found
  }

  return models.size() == 1 ? models[0] : z3::mk_or(models);
}

Int2BvPreprocessor::Int2BvPreprocessor(z3::context &c, unsigned bv_size,
                                       bool is_signed)
    : m_ctx(c), m_bv_size(bv_size), m_is_signed(is_signed),
      m_vars_bound_lemmas(c) {}

void Int2BvPreprocessor::reset() {
  m_handled_vars.clear();
  m_vars_bound_lemmas = z3::expr_vector(m_ctx);
  m_const_out_of_bounds.clear();
  m_func_app_out_of_bounds.clear();
  m_literals.clear();
}

bool Int2BvPreprocessor::is_const_variable(const z3::expr &e) const {
  // Check if the expression is a variable (0-arity application)
  return e.is_const() && !e.is_numeral() && !e.is_bool();
}

z3::expr
Int2BvPreprocessor::create_term_out_of_bounds_expr(const z3::expr &e) const {
  Z3_decl_kind e_kind = e.decl().decl_kind();
  z3::expr res(m_ctx);

  if (m_is_signed) {
    int64_t lower_bound = utils::get_signed_bv_lower_bound(m_bv_size);
    switch (e_kind) {
    case Z3_OP_ADD:
    case Z3_OP_SUB:
    case Z3_OP_MUL:
      res = !create_bounds_expr(e);
      break;
    case Z3_OP_UMINUS:
      res = (e.arg(0) == m_ctx.int_val(lower_bound));
      break;
    case Z3_OP_DIV:
    case Z3_OP_IDIV:
      res = ((e.arg(0) == m_ctx.int_val(lower_bound)) &&
             (e.arg(1) == m_ctx.int_val(-1)));
      break;
    default:
      MOSAIC_UNREACHABLE();
    }
  } else {
    uint64_t upper_bound = utils::get_unsigned_bv_upper_bound(m_bv_size);
    switch (e_kind) {
    case Z3_OP_ADD:
      res = (e > m_ctx.int_val(upper_bound));
      break;
    case Z3_OP_SUB:
      res = (e.arg(0) < e.arg(1));
      break;
    case Z3_OP_MUL:
      res = (e > m_ctx.int_val(upper_bound));
      break;
    case Z3_OP_UMINUS:
      res = (e.arg(0) != m_ctx.int_val(0));
      break;
    default:
      MOSAIC_UNREACHABLE();
    }
  }
  return res;
}

void Int2BvPreprocessor::handle_term(
    const z3::expr &e, z3::expr_vector &func_app_out_of_bounds) const {
  Z3_decl_kind e_kind = e.decl().decl_kind();
  if (m_is_signed) {
    // Handle signed operations
    if (is_signed_relevant_func_app(e_kind)) {
      z3::expr out_of_bounds_expr = create_term_out_of_bounds_expr(e);
      func_app_out_of_bounds.push_back(out_of_bounds_expr);
    }
  }

  if (is_unsigned_relevant_func_app(e_kind)) {
    // Handle unsigned operations
    z3::expr out_of_bounds_expr = create_term_out_of_bounds_expr(e);
    func_app_out_of_bounds.push_back(out_of_bounds_expr);
  }
}

z3::expr Int2BvPreprocessor::create_bounds_expr(const z3::expr &term) const {
  if (m_is_signed) {
    int64_t N = (int64_t)1 << (m_bv_size - 1);
    return (term >= m_ctx.int_val(-N)) && (term <= m_ctx.int_val(N - 1));
  }

  int64_t N = (uint64_t)1 << m_bv_size;
  return (term >= m_ctx.int_val(0)) && (term <= m_ctx.int_val(N - 1));
}

bool Int2BvPreprocessor::is_const_in_bounds(const z3::expr &const_e) const {
  assert(const_e.is_numeral() && "Expected a constant expression");
  if (m_is_signed) {
    int64_t lower_bound = utils::get_signed_bv_lower_bound(m_bv_size);
    int64_t upper_bound = utils::get_signed_bv_upper_bound(m_bv_size);
    return const_e.get_numeral_int64() >= lower_bound &&
           const_e.get_numeral_int64() <= upper_bound;
  }

  uint64_t upper_bound = utils::get_unsigned_bv_upper_bound(m_bv_size);
  uint64_t lower_bound = utils::get_unsigned_bv_lower_bound(m_bv_size);
  assert(m_bv_size <= 64 && "Unexpected bv size");
  return const_e.get_numeral_int64() >= lower_bound &&
         const_e.get_numeral_int64() <= upper_bound;
}

void Int2BvPreprocessor::populate_data_structures(const z3::expr &e) {
  // TODO: Make sure the code below is correct as "and" expressions are nested.
  int n_conjuncts = utils::get_num_conjuncts(e);

  m_const_out_of_bounds.resize(n_conjuncts);
  m_func_app_out_of_bounds.resize(n_conjuncts);
  m_literals.resize(n_conjuncts);

  for (int i = 0; i < n_conjuncts; ++i) {
    z3::expr conjunct = (n_conjuncts == 1) ? e : e.arg(i);
    // TODO: Make sure the code below is correct as "or" expressions are nested.
    int n_disjuncts = utils::get_num_disjuncts(conjunct);

    m_const_out_of_bounds[i].resize(n_disjuncts, false);
    m_func_app_out_of_bounds[i].resize(n_disjuncts, z3::expr_vector(m_ctx));
    m_literals[i].resize(n_disjuncts, z3::expr(m_ctx));

    for (int j = 0; j < n_disjuncts; ++j) {
      z3::expr literal = (n_disjuncts == 1) ? conjunct : conjunct.arg(j);
      z3::expr_vector literal_func_app_out_of_bounds(m_ctx);
      bool literal_const_out_of_bounds = false;

      analyze_literal(literal, literal_const_out_of_bounds,
                      literal_func_app_out_of_bounds);

      m_literals[i][j] = literal;
      m_const_out_of_bounds[i][j] = literal_const_out_of_bounds;
      m_func_app_out_of_bounds[i][j] = literal_func_app_out_of_bounds;
    }
  }
}

void Int2BvPreprocessor::analyze_literal(
    const z3::expr &literal, bool &const_out_of_bounds,
    z3::expr_vector &func_app_out_of_bounds) {
  if (literal.is_true() || literal.is_false()) {
    return;
  } else if (literal.is_numeral()) {
    bool in_bounds = is_const_in_bounds(literal);
    const_out_of_bounds |= !in_bounds;
  } else if (is_const_variable(literal) &&
             m_handled_vars.find((Z3_ast)literal) == m_handled_vars.end()) {
    // Add the variable to the cache if not already handled
    m_handled_vars.insert((Z3_ast)literal);
    z3::expr var_bound_expr = create_bounds_expr(literal);
    m_vars_bound_lemmas.push_back(var_bound_expr);
  } else {
    handle_term(literal, func_app_out_of_bounds);

    for (unsigned i = 0; i < literal.num_args(); ++i) {
      analyze_literal(literal.arg(i), const_out_of_bounds,
                      func_app_out_of_bounds);
    }
  }
}

int Int2BvPreprocessor::get_num_of_conjuncts() const {
  assert(m_literals.size() > 0 && "Data structures not populated");
  return m_literals.size();
}

int Int2BvPreprocessor::get_num_of_disjuncts(int conjunct) const {
  assert(conjunct >= 0 && conjunct < m_literals.size() &&
         "Invalid conjunct index");
  return m_literals[conjunct].size();
}

z3::expr
Int2BvPreprocessor::create_SAT_out_of_bounds_expr(const z3::expr &e) const {
  z3::expr bounds = z3::mk_and(m_vars_bound_lemmas);
  z3::expr rest = m_ctx.bool_val(false);
  int n_conjuncts = get_num_of_conjuncts();

  for (int i = 0; i < n_conjuncts; ++i) {
    int n_disjuncts = get_num_of_disjuncts(i);
    bool exists_const_in_bounds = false;
    for (int j = 0; j < n_disjuncts; ++j) {
      if (!m_const_out_of_bounds[i][j]) {
        exists_const_in_bounds = true;
        break;
      }
    }

    // An optimization instead of creating a large expression
    if (!exists_const_in_bounds) {
      continue;
    } else {
      z3::expr conjuncts = m_ctx.bool_val(true);
      for (int j = 0; j < n_disjuncts; ++j) {
        if (m_const_out_of_bounds[i][j]) {
          continue;
        }

        conjuncts = conjuncts && (!m_literals[i][j] ||
                                  z3::mk_or(m_func_app_out_of_bounds[i][j]));
      }
      rest = rest || conjuncts;
    }
  }
  z3::expr res(m_ctx);
  res = e && bounds && rest;
  return better_simplify(res);
}

z3::expr
Int2BvPreprocessor::create_UNSAT_out_of_bounds_expr(const z3::expr &e) const {
  z3::expr bounds = z3::mk_and(m_vars_bound_lemmas);
  z3::expr rest = m_ctx.bool_val(true);
  int n_conjuncts = get_num_of_conjuncts();

  for (int i = 0; i < n_conjuncts; ++i) {
    int n_disjuncts = get_num_of_disjuncts(i);
    bool all_consts_out_of_bounds = true;
    for (int j = 0; j < n_disjuncts; ++j) {
      if (!m_const_out_of_bounds[i][j]) {
        all_consts_out_of_bounds = false;
        break;
      }
    }

    // An optimization instead of creating a large expression
    if (all_consts_out_of_bounds) {
      continue;
    } else {
      z3::expr conjunct = m_ctx.bool_val(false);
      for (int j = 0; j < n_disjuncts; ++j) {
        conjunct = conjunct || m_literals[i][j];
      }
      z3::expr disjuncts = m_ctx.bool_val(false);
      for (int j = 0; j < n_disjuncts; ++j) {
        if (m_const_out_of_bounds[i][j]) {
          continue;
        }

        disjuncts = disjuncts || z3::mk_or(m_func_app_out_of_bounds[i][j]);
      }
      rest = rest && (conjunct || disjuncts);
    }
  }
  z3::expr res(m_ctx);
  res = !e && bounds && rest;
  return better_simplify(res);
}

bool Int2BvPreprocessor::all_is_well(const z3::expr &e) const {
  if (e.is_app()) {
    Z3_decl_kind e_kind = e.decl().decl_kind();
    if (m_is_signed && is_signed_relevant_func_app(e_kind)) {
      return false;
    }
    if (!m_is_signed && is_unsigned_relevant_func_app(e_kind)) {
      return false;
    }
  } else if (e.is_numeral()) {
    // If it's a constant, check if it's within bounds
    return is_const_in_bounds(e);
  }
  // Recursively check all arguments
  for (unsigned i = 0; i < e.num_args(); ++i) {
    if (!all_is_well(e.arg(i))) {
      return false;
    }
  }
  return true;
}

z3::expr Int2BvPreprocessor::create_psi_expr(const z3::expr &e) const {
  if (all_is_well(e)) {
    return e;
  }

  return create_formula_from_models(e);
}

z3::expr
Int2BvPreprocessor::drop_out_of_bounds_literals(const z3::expr &e) const {
  z3::expr_vector new_clauses(m_ctx);
  for (unsigned i = 0; i < m_literals.size(); ++i) {
    z3::expr_vector new_literals(m_ctx);
    for (unsigned j = 0; j < m_literals[i].size(); ++j) {
      if (!m_const_out_of_bounds[i][j]) {
        new_literals.push_back(m_literals[i][j]);
      }
    }
    if (new_literals.empty())
      continue;
    z3::expr disjunct =
        new_literals.size() == 1 ? new_literals[0] : z3::mk_or(new_literals);
    new_clauses.push_back(disjunct);
  }
  if (new_clauses.empty())
    return m_ctx.bool_val(true);
  return new_clauses.size() == 1 ? new_clauses[0] : z3::mk_and(new_clauses);
}

z3::expr Int2BvPreprocessor::create_SAT_out_of_bounds(const z3::expr &e) {
  populate_data_structures(e);
  return create_SAT_out_of_bounds_expr(e);
}

z3::expr Int2BvPreprocessor::create_UNSAT_out_of_bounds(const z3::expr &e) {
  populate_data_structures(e);
  return create_UNSAT_out_of_bounds_expr(e);
}

z3::expr Int2BvPreprocessor::preprocess(const z3::expr &e) {
  InstCombiner combiner = InstCombiner(m_ctx);
  // Help the preprocessor by optimizing the expressions
  // to more "friendly" forms
  z3::expr e_opt = combiner.combine(e);
  LDBG("Combined input: " << e_opt << "\n");
  // Assume input is in CNF
  populate_data_structures(e_opt);
  z3::expr psi = create_SAT_out_of_bounds_expr(e_opt);
  LDBG("psi (SATOutOfBounds): " << psi << "\n");
  z3::expr psi_tag = create_UNSAT_out_of_bounds_expr(e_opt);
  LDBG("psi_tag (UNSATOutOfBounds): " << psi_tag << "\n");

  z3::expr psi_SAT = create_psi_expr(psi);
  LDBG("psi_SAT: " << psi_SAT << "\n");
  z3::expr psi_UNSAT = create_psi_expr(psi_tag);
  LDBG("psi_UNSAT: " << psi_UNSAT << "\n");
  z3::expr phi_1 = drop_out_of_bounds_literals(e_opt);
  LDBG("phi_1: " << phi_1 << "\n");
  z3::expr phi_2 = (phi_1 && !psi_UNSAT) || psi_SAT;
  LDBG("phi_2: " << phi_2 << "\n");

  return better_simplify(phi_2);
}
} // namespace lotus::mosaic