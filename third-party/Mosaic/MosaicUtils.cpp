#include "Mosaic/MosaicUtils.h"

namespace lotus::mosaic {
static bool is_signed_bv_op(Z3_decl_kind k) {
  switch (k) {
  case Z3_OP_SLEQ:
  case Z3_OP_SGEQ:
  case Z3_OP_SLT:
  case Z3_OP_SGT:
  case Z3_OP_BSDIV:
  case Z3_OP_BSREM:
  case Z3_OP_BSMOD:
  case Z3_OP_BSMUL_NO_OVFL:
  case Z3_OP_BSMUL_NO_UDFL:
  case Z3_OP_BSDIV_I:
    return true;
  default:
    return false;
  }
}

static void set_signedness(Signedness &current, bool is_signed,
                           bool is_unsigned) {
  assert(!(is_signed && is_unsigned) &&
         "Expression cannot be both signed and unsigned");

  if (is_signed) {
    if (current == Signedness::UNKNOWN) {
      current = Signedness::SIGNED;
    } else if (current == Signedness::UNSIGNED) {
      current = Signedness::CONFLICT;
    }
  } else if (is_unsigned) {
    if (current == Signedness::UNKNOWN) {
      current = Signedness::UNSIGNED;
    } else if (current == Signedness::SIGNED) {
      current = Signedness::CONFLICT;
    }
  } else {
    // Otherwise, don't change the current signedness
    return;
  }
}

static bool is_unsigned_bv_op(Z3_decl_kind k) {
  switch (k) {
  case Z3_OP_ULEQ:
  case Z3_OP_UGEQ:
  case Z3_OP_ULT:
  case Z3_OP_UGT:
  case Z3_OP_BUDIV:
  case Z3_OP_BUREM:
  case Z3_OP_BUMUL_NO_OVFL:
  case Z3_OP_BUDIV_I:
    return true;
  default:
    return false;
  }
}

static bool is_bit_manipulating_bv_op(Z3_decl_kind k) {
  switch (k) {
  case Z3_OP_CONCAT:
  case Z3_OP_EXTRACT:
  case Z3_OP_REPEAT:
  case Z3_OP_BAND:
  case Z3_OP_BOR:
  case Z3_OP_BNOT:
  case Z3_OP_BXOR:
  case Z3_OP_BNAND:
  case Z3_OP_BNOR:
  case Z3_OP_BXNOR:
  case Z3_OP_SIGN_EXT:
  case Z3_OP_ZERO_EXT:
  case Z3_OP_BSHL:
  case Z3_OP_BLSHR:
  case Z3_OP_BASHR:
  case Z3_OP_ROTATE_LEFT:
  case Z3_OP_ROTATE_RIGHT:
  case Z3_OP_EXT_ROTATE_LEFT:
  case Z3_OP_EXT_ROTATE_RIGHT:
    return true;
  default:
    return false;
  }
}

static bool is_sign_neutral_bv_op(Z3_decl_kind k) {
  return (Z3_OP_BNUM <= k && k <= Z3_OP_BMUL) ||
         (Z3_OP_TRUE <= k && k <= Z3_OP_OEQ);
}

// This function is a "hack" to extract the variable index from a z3::expr
// variable. The Z3 C++ API does not expose a direct method to get the variable
// index. Therefore, we convert the variable to string and parse the index. When
// printing a variable, Z3 uses the format (:var <index>)
static int extract_var_index(z3::expr const &var) {
  assert(var.is_var() && "Expression is not a variable");
  std::string var_str = var.to_string();
  size_t start_pos = var_str.find(":var ") + 5; // Length of ":var " is 5
  size_t end_pos = var_str.find(")", start_pos);
  std::string index_str = var_str.substr(start_pos, end_pos - start_pos);
  return std::stoi(index_str);
}

static void analyze_expr(z3::expr const &e, bool in_pred_body,
                         ClauseAnalysisResult &result) {
  if (e.is_quantifier()) {
    // In case of horn clauses, this shouldn't be reached
    // This should be unrecahable as quantifiers should not be present
    // in the CHC body.
    MOSAIC_UNREACHABLE();
  } else if (e.is_var()) {
    result.all_vars.insert(e);
    int var_index = extract_var_index(e);
    result.var_index_map.emplace(e, var_index);
    if (in_pred_body) {
      result.in_pred_body_vars.insert(e);
    }
  } else {
    // is_app
    if (e.is_const())
      return;

    // Analyze signedness and bit-manipulating ops
    Z3_decl_kind k = e.decl().decl_kind();
    bool is_sign_neutral = is_sign_neutral_bv_op(k);
    bool is_signed = is_signed_bv_op(k);
    bool is_unsigned = is_unsigned_bv_op(k);
    bool is_bit_manipulating = is_bit_manipulating_bv_op(k);
    bool is_sign_known = is_signed || is_unsigned;
    assert(is_sign_known || is_sign_neutral ||
           is_bit_manipulating && "Dealing with an unknown application");
    if (is_bit_manipulating) {
      result.has_bit_manipulating_apps = true;
    }

    set_signedness(result.is_signed, is_signed, is_unsigned);

    for (unsigned i = 0; i < e.num_args(); ++i) {
      analyze_expr(e.arg(i), in_pred_body, result);
    }
  }
}

static void analyze_uninterpreted_predicate(z3::expr const &e, bool is_in_body,
                                            ClauseAnalysisResult &result) {
  for (unsigned i = 0; i < e.num_args(); ++i) {
    analyze_expr(e.arg(i), /*in_pred_body*/ is_in_body, result);
  }
}

static void analyze_clause_body(z3::expr const &body,
                                ClauseAnalysisResult &result) {
  z3::expr_vector conjuncts = utils::get_conjuncts(body);
  int conjunct_count = conjuncts.size();
  for (int i = 0; i < conjunct_count; ++i) {
    z3::expr conjunct = conjuncts[i];
    // Check if the conjunct is a predicate application
    if (utils::is_uninterpreted_predicate(conjunct)) {
      analyze_uninterpreted_predicate(conjunct, /*is_in_body*/ true, result);
    } else {
      analyze_expr(conjunct, /*in_pred_body*/ false, result);
    }
  }
}

static void analyze_rule(z3::expr const &body, z3::expr const &head,
                         ClauseAnalysisResult &result) {
  analyze_clause_body(body, result);
  assert(utils::is_uninterpreted_predicate(head) &&
         "The head of a rule must be an uninterpreted predicate");
  analyze_uninterpreted_predicate(head, /*is_in_body*/ false, result);
}

static void analyze_query(z3::expr const &body, ClauseAnalysisResult &result) {
  analyze_clause_body(body, result);
}

// Checks if all vars have the same bv_size, and sets result.bv_size
// accordingly. If not, result.bv_size is set to UNDETERM_BV_SIZE.
static void analyze_vars(z3::expr_vector const &vars,
                         ClauseAnalysisResult &result) {
  unsigned common_bv_size = UNDETERM_BV_SIZE;
  bool first = true;
  for (unsigned i = 0; i < vars.size(); ++i) {
    z3::sort var_sort = vars[i].get_sort();
    assert(var_sort.is_bv() && "Variable is expected to be of bv sort");
    unsigned var_bv_size = var_sort.bv_size();
    assert(var_bv_size > 0 && "Bit-vector size must be greater than 0");
    if (first) {
      common_bv_size = var_bv_size;
      first = false;
      continue;
    }
    if (var_bv_size != common_bv_size) {
      common_bv_size = UNDETERM_BV_SIZE;
      break;
    }
  }
  result.bv_size = common_bv_size;
}

//==============================================================================
//                              PUBLIC METHODS
//==============================================================================
z3::expr_vector get_clause_vars(z3::expr const &clause) {
  z3::context &c = clause.ctx();
  z3::expr_vector vars(c);
  for (int j = 0; j < Z3_get_quantifier_num_bound(c, clause); j++) {
    z3::symbol sym = z3::symbol(c, Z3_get_quantifier_bound_name(c, clause, j));
    z3::expr var = c.constant(
        sym, z3::sort(c, Z3_get_quantifier_bound_sort(c, clause, j)));
    vars.push_back(var);
  }
  return vars;
}

ClauseAnalysisResult analyze_clause(z3::context &ctx, z3::expr const &clause) {
  assert(clause.is_quantifier() && "The clause must be a quantifier");
  ClauseAnalysisResult result(ctx);

  z3::expr_vector vars = get_clause_vars(clause);
  analyze_vars(vars, result);

  if (clause.is_exists()) {
    assert(clause.body().is_and() && "The clause body must be a conjunction");
    z3::expr body = clause.body();
    analyze_query(body, result);
  } else if (clause.is_forall()) {
    z3::expr body = get_clause_body(clause, /*is_query*/ false);
    z3::expr head = get_clause_head(clause, /*is_query*/ false);

    // Theoretically, a query can be a forall clause that implies false.
    // Implement only if needed.
    if (head.is_false())
      MOSAIC_NOT_IMPLEMENTED();

    analyze_rule(body, head, result);
  } else {
    // Clause should be a forall or exists clause
    MOSAIC_UNREACHABLE();
  }

  return result;
}

z3::expr get_clause_head(z3::expr const &clause, bool is_query) {
  if (is_query) {
    assert(clause.is_quantifier() && clause.is_exists() &&
           "The clause must be an exists quantifier");
    assert(clause.body().is_and() && "The clause body must be a conjunction");
    // Queries do not have heads
    return z3::expr(clause.ctx());
  }
  assert(clause.is_quantifier() && clause.is_forall() &&
         "The clause must be a forall quantifier");
  assert(clause.body().is_implies() &&
         "The clause body must be an implication");
  return clause.body().arg(1);
}

z3::expr get_clause_body(z3::expr const &clause, bool is_query) {
  if (is_query) {
    assert(clause.is_quantifier() && clause.is_exists() &&
           "The clause must be an exists quantifier");
    assert(clause.body().is_and() && "The clause body must be a conjunction");
    return clause.body();
  }
  assert(clause.is_quantifier() && "The clause must be a quantifier");
  assert(clause.body().is_implies() &&
         "The clause body must be an implication");
  return clause.body().arg(0);
}

z3::params get_default_mth_fp_params(z3::context &ctx) {
  z3::params params(ctx);
  params.set("engine", "spacer");
  params.set("fp.xform.slice", false);
  params.set("fp.xform.inline_linear", false);
  params.set("fp.xform.inline_eager", false);
  return params;
}

static z3::expr evaluate_clause_vars_internal(z3::expr const &expr,
                                              VarMap &unknown_vars_subs,
                                              unsigned &unknown_vars) {
  if (expr.is_var()) {
    if (unknown_vars_subs.find(expr) != unknown_vars_subs.end())
      return unknown_vars_subs.at(expr);
    z3::sort sort = expr.get_sort();
    std::string var_name = "tmp_var_" + std::to_string(unknown_vars++);
    z3::expr new_var = expr.ctx().constant(var_name.c_str(), sort);
    unknown_vars_subs.emplace(expr, new_var);
    return new_var;
  }

  if (expr.num_args() == 0)
    return expr;

  z3::expr_vector args = utils::get_args(expr);
  z3::expr_vector new_args(expr.ctx());
  for (unsigned i = 0; i < args.size(); ++i) {
    z3::expr evaluated_arg =
        evaluate_clause_vars_internal(args[i], unknown_vars_subs, unknown_vars);
    new_args.push_back(evaluated_arg);
  }

  return expr.decl()(new_args);
}

z3::expr evaluate_clause_vars(z3::expr const &expr, VarMap &unknown_vars_subs) {
  unsigned unknown_vars = 0;
  return evaluate_clause_vars_internal(expr, unknown_vars_subs, unknown_vars);
}

// ====================================================================
// MTHSolver methods
// ====================================================================
z3::expr_vector MTHSolver::get_all_clauses() const {
  z3::expr_vector clauses = fp_solver.rules();
  if (query)
    clauses.push_back(query);
  return clauses;
}

unsigned MTHSolver::get_bv_size() const {
  assert(!is_bv && "This method should only be called for IAUF solvers");
  assert(bv_size != UNDETERMINED_BV_SIZE &&
         "Bit-vector size should be determined");
  return bv_size;
}

// ====================================================================
// MTHFixedpointSet methods
// ====================================================================
bool MTHFixedpointSet::check_and_set_signedness(bool incoming_sign) {
  if (!global_is_signed.has_value()) {
    global_is_signed = incoming_sign;
    return true;
  }
  return global_is_signed.value() == incoming_sign;
}

bool MTHFixedpointSet::hasBVSolver() const { return bv_solver.has_value(); }

MTHSolver &MTHFixedpointSet::getOrInitBVSolver() {
  if (!bv_solver.has_value()) {
    MTHSolver new_solver(ctx, /*is_bv*/ true);
    bv_solver.emplace(std::move(new_solver));
  }
  return *bv_solver;
}

MTHSolver &MTHFixedpointSet::getBVSolver() {
  assert(bv_solver.has_value() && "BV Solver not found.");
  return *bv_solver;
}

bool MTHFixedpointSet::hasIAUFSolver(unsigned bv_size) const {
  return iauf_solvers.find(bv_size) != iauf_solvers.end();
}

MTHSolver &MTHFixedpointSet::getOrInitIAUFSolver(unsigned bv_size) {
  auto it = iauf_solvers.find(bv_size);
  if (it != iauf_solvers.end())
    return it->second;

  MTHSolver new_solver(ctx, /*is_bv*/ false, bv_size);
  auto result = iauf_solvers.emplace(
      std::piecewise_construct, std::forward_as_tuple(bv_size),
      std::forward_as_tuple(std::move(new_solver)));

  return result.first->second;
}

MTHSolver &MTHFixedpointSet::getIAUFSolver(unsigned bv_size) {
  auto it = iauf_solvers.find(bv_size);
  assert(it != iauf_solvers.end() &&
         "IAUF Solver not found for the requested bv_size.");
  return it->second;
}

z3::symbol MTHFixedpointSet::get_fresh_rule_name() {
  std::string name_str = rule_name + std::to_string(rule_count);
  rule_count++;
  return ctx.str_symbol(name_str.c_str());
}

void MTHFixedpointSet::populate_predicate_maps_for_clause(
    const z3::expr clause_expr, MTHSolver *solver, bool is_query) {
  unsigned clause_id = clause_expr.id();
  z3::expr body = get_clause_body(clause_expr, is_query);
  z3::expr head = get_clause_head(clause_expr, is_query);
  if (bool(head))
    pred_solver_map.emplace(head.decl(), solver);
  z3::expr_vector conjuncts = utils::get_conjuncts(body);
  z3::expr_vector body_preds(ctx);
  int conjunct_count = conjuncts.size();
  for (int i = 0; i < conjunct_count; ++i) {
    z3::expr conjunct = conjuncts[i];
    // Check if the conjunct is a predicate application
    if (utils::is_uninterpreted_predicate(conjunct))
      body_preds.push_back(conjunct);
    pred_solver_map.emplace(conjunct.decl(), solver);
  }
  rule_body_preds_map.emplace(clause_id, body_preds);
  rule_head_pred_map.emplace(clause_id, head);
}

std::optional<z3::expr_vector>
MTHFixedpointSet::get_clause_body_preds(const z3::expr &clause) const {
  auto it = rule_body_preds_map.find(clause.id());
  if (it != rule_body_preds_map.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<z3::expr>
MTHFixedpointSet::get_clause_head_pred(const z3::expr &clause) const {
  auto it = rule_head_pred_map.find(clause.id());
  if (it != rule_head_pred_map.end()) {
    return it->second;
  }
  return std::nullopt;
}

void MTHFixedpointSet::map_clause_to_solver(const z3::expr &clause,
                                            MTHSolver *solver) {
  rule_solver_map.emplace(clause.id(), solver);
}

MTHSolver *MTHFixedpointSet::get_clause_solver(const z3::expr &clause) const {
  auto it = rule_solver_map.find(clause.id());
  assert(it != rule_solver_map.end() &&
         "Solver not found for the given clause.");
  return it->second;
}

MTHSolver *
MTHFixedpointSet::get_predicate_solver(const z3::func_decl &pred) const {
  auto it = pred_solver_map.find(pred);
  assert(it != pred_solver_map.end() &&
         "Solver not found for the given predicate.");
  return it->second;
}

std::ostream &operator<<(std::ostream &os, const MTHFixedpointSet &mth_fp_set) {
  os << "MTHFixedpointSet - ";
  if (mth_fp_set.global_is_signed.has_value()) {
    os << "(Global Signedness: "
       << (mth_fp_set.global_is_signed.value() ? "SIGNED" : "UNSIGNED") << ")"
       << std::endl;
  } else {
    os << "(Global Signedness: UNDETERMINED)" << std::endl;
  }

  if (mth_fp_set.bv_solver.has_value()) {
    os << "======== BV Solver (Initialized):" << std::endl;
    os << (mth_fp_set.bv_solver).value().fp_solver;
    os << "======== Query: " << std::endl;
    os << (mth_fp_set.bv_solver).value().query << std::endl;
  } else {
    os << "======== BV Solver (Not Initialized)" << std::endl;
  }
  os << std::endl;

  os << "IAUF Solvers:" << std::endl;
  if (mth_fp_set.iauf_solvers.empty()) {
    os << "    None" << std::endl;
  } else {
    for (const auto &[bv_size, solver] : mth_fp_set.iauf_solvers) {
      os << "======== BV Size " << bv_size << ": Initialized" << std::endl;
      os << solver.fp_solver;
      os << "======== Query: " << std::endl;
      os << solver.query << std::endl;
    }
  }
  return os;
}

// ====================================================================
// ClauseAnalysisResult methods
// ====================================================================
std::ostream &operator<<(std::ostream &os, const ClauseAnalysisResult &result) {
  os << "Clause Analysis Result:" << std::endl;
  os << "  Signedness: ";
  switch (result.is_signed) {
  case Signedness::UNKNOWN:
    os << "UNKNOWN";
    break;
  case Signedness::SIGNED:
    os << "SIGNED";
    break;
  case Signedness::UNSIGNED:
    os << "UNSIGNED";
    break;
  case Signedness::CONFLICT:
    os << "CONFLICT";
    break;
  }
  os << std::endl;

  os << "  BV Size: ";
  if (result.is_bv_size_determined()) {
    os << result.bv_size;
  } else {
    os << "UNDETERMINED";
  }
  os << std::endl;

  os << "  Has Bit-Manipulating Apps: "
     << (result.has_bit_manipulating_apps ? "Yes" : "No") << std::endl;

  os << "  In Predicate Body: ";
  int num_in_pred_body = result.in_pred_body_vars.size();
  for (int i = 0; i < num_in_pred_body; ++i) {
    auto var = *std::next(result.in_pred_body_vars.begin(), i);
    os << var;
    if (i != num_in_pred_body - 1) {
      os << ", ";
    }
  }
  os << std::endl;

  os << "  All Variables: ";
  int num_all_vars = result.all_vars.size();
  for (int i = 0; i < num_all_vars; ++i) {
    auto var = *std::next(result.all_vars.begin(), i);
    os << var;
    if (i != num_all_vars - 1) {
      os << ", ";
    }
  }
  os << std::endl;

  os << "  Variable Index Map: " << std::endl;
  for (const auto &[var, index] : result.var_index_map) {
    os << "    " << var << " -> " << index << std::endl;
  }
  os << std::endl;
  return os;
}
} // namespace lotus::mosaic
