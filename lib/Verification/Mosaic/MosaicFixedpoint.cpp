//------------------------------------------------------------------------------
//  multi_theory_fixedpoint.cpp
//------------------------------------------------------------------------------
#include "Verification/Mosaic/MosaicFixedpoint.h"

namespace lotus::mosaic {

static bool is_hyper_res(z3::expr const &e) {
  return e.decl().decl_kind() == Z3_OP_PR_HYPER_RESOLVE;
}

/// @brief Checks if the given predicate is user-visible according to its name.
/// A non-visitble predicate is one that is created by the fixedpoint
/// engine to represent a refutation and is of the form `query!X` where `X` is a
/// number.
/// @param p The predicate to check.
/// @return true if the predicate is user-visible, i.e. created by the user and
/// false otherwise.
static bool is_user_visible_predicate(z3::func_decl const &p) {
  std::string p_name = p.name().str();

  // Check if p has a name of the form query!X where X is a number
  if (p_name.find("query!") == 0) {
    // Check that the rest of the string is a number
    std::string num_str = p_name.substr(6);
    if (std::all_of(num_str.begin(), num_str.end(), ::isdigit)) {
      return false;
    }
  }

  return true;
}

// Concatenates the bv_size to the name of the given predicate.
static std::string create_int_predicate_name(const z3::func_decl &p_old,
                                             unsigned bv_size) {
  return p_old.name().str() + "_" + std::to_string(bv_size);
}

// Extracts the original predicate name by removing the bv_size suffix.
static std::string extract_old_bv_predicate_name(const z3::func_decl &p_new,
                                                 unsigned bv_size) {
  std::string suffix = "_" + std::to_string(bv_size);
  std::string p_name_str = p_new.name().str();
  assert(p_name_str.size() > suffix.size() &&
         p_name_str.substr(p_name_str.size() - suffix.size()) == suffix &&
         "Predicate name does not have the expected suffix");
  return p_name_str.substr(0, p_name_str.size() - suffix.size());
}

// Creates a copy of the given predicate with the same name but with bv_size
// concatenated to it.
// The sort and range of the old predicate should be bit-vectors and bool
// respectively. The new predicate will have int sorts for its arguments.
static z3::func_decl bv_predicate_to_int(const z3::func_decl &p_old,
                                         unsigned bv_size) {
  z3::context &ctx = p_old.ctx();
  std::string p_name_str = create_int_predicate_name(p_old, bv_size);
  z3::symbol p_name = ctx.str_symbol(p_name_str.c_str());

  // Create the new argument sorts
  unsigned arity = p_old.arity();
  std::vector<z3::sort> arg_sorts;
  for (unsigned i = 0; i < arity; ++i) {
    z3::sort old_sort = p_old.domain(i);
    assert(old_sort.is_bv() && "Sort should be BV");
    arg_sorts.push_back(ctx.int_sort());
  }

  // Create the new return sort
  assert(p_old.range().is_bool() && "Predicate return sort should be bool");
  z3::sort ret_sort = p_old.range();

  // Create the new predicate
  z3::func_decl p_new = ctx.function(p_name, arity, arg_sorts.data(), ret_sort);
  return p_new;
}

static VarMap
build_bv2int_var_map(z3::context &ctx, z3::expr_vector &bv_vars_exprs,
                     const ClauseAnalysisResult &clause_analysis) {
  VarMap bv_to_int_var_map;
  for (const z3::expr &var_bv : clause_analysis.all_vars) {
    int var_index = clause_analysis.var_index_map.at(var_bv);
    std::string var_name = bv_vars_exprs[var_index].decl().name().str();
    // Create a new corresponding variable of int sort
    z3::expr new_var_int = ctx.int_const(var_name.c_str());
    // Emplace the variable in the map.
    bv_to_int_var_map.emplace(var_bv, new_var_int);
  }
  return bv_to_int_var_map;
}

static z3::expr
translate_BV_CHC_rule(const z3::expr &clause,
                      const ClauseAnalysisResult &clause_analysis,
                      Bv2IntTranslator &translator, bool is_query) {
  MOSAIC_DEBUG_MSG(OUT() << "Translating BV CHC clause: " << clause << std::endl
                         << "With BV to int VarMap:\n"
                         << dump(translator.bv2int_var_map()) << std::endl);
  z3::context &ctx = clause.ctx();
  const VarMap &new_vars_map = translator.bv2int_var_map();

  unsigned num_vars = clause_analysis.all_vars.size();
  z3::expr_vector new_vars(ctx);
  new_vars.resize(num_vars);
  for (const z3::expr &var_bv : clause_analysis.all_vars) {
    int var_index = clause_analysis.var_index_map.at(var_bv);
    assert(var_index >= 0 && static_cast<unsigned>(var_index) < num_vars);
    z3::expr tmp = new_vars_map.at(var_bv);
    new_vars.set(var_index, tmp);
  }

  z3::expr body = get_clause_body(clause, /*is_query*/ is_query);
  z3::expr head = get_clause_head(clause, /*is_query*/ is_query);

  z3::expr_vector conjuncts = utils::get_conjuncts(body);
  int num_conjuncts = conjuncts.size();
  z3::expr_vector new_conjunct(ctx);
  for (int i = 0; i < num_conjuncts; ++i) {
    z3::expr conjunct = conjuncts[i];
    z3::expr translated_conjunct(ctx);
    if (utils::is_uninterpreted_predicate(conjunct)) {
      z3::func_decl p_new =
          bv_predicate_to_int(conjunct.decl(), clause_analysis.bv_size);
      z3::expr translated_arg(ctx);
      z3::expr_vector new_args(ctx);
      for (unsigned j = 0; j < conjunct.num_args(); ++j) {
        z3::expr arg = conjunct.arg(j);
        translated_arg = translator.translate(arg);
        new_args.push_back(translated_arg);
      }
      translated_conjunct = p_new(new_args);
    } else {
      translated_conjunct = translator.translate(conjunct);
    }
    new_conjunct.push_back(translated_conjunct);
  }

  // Go over all_vars and check if the variable is NOT in a predicate's app
  // body. If it is not in a predicate's app body, then we need to add bounds
  // for it.
  VarLemmaMap const &var_lemmas = translator.lemmas();
  for (const z3::expr &var_bv : clause_analysis.all_vars) {
    z3::expr var_int = new_vars_map.at(var_bv);
    if (clause_analysis.in_pred_body_vars.find(var_bv) ==
        clause_analysis.in_pred_body_vars.end()) {
      // Variable is not in a predicate body, add its bound lemma
      auto it = var_lemmas.find(var_int);
      assert(it != var_lemmas.end() && "Lemma for variable not found");
      new_conjunct.push_back(it->second);
    }
  }

  // Create the new body expression
  z3::expr new_body = z3::mk_and(new_conjunct);

  if (is_query) {
    z3::expr new_query = z3::exists(new_vars, new_body);
    MOSAIC_DEBUG_MSG(OUT() << "Translated BV (size " << clause_analysis.bv_size
                           << ") CHC query to int CHC query: " << new_query
                           << std::endl);
    return new_query;
  }

  // If we're dealing with a normal rule, also translate the head
  z3::expr new_head(ctx);
  assert(utils::is_uninterpreted_predicate(head) &&
         "Head must be an uninterpreted predicate");
  z3::func_decl p_head_new =
      bv_predicate_to_int(head.decl(), clause_analysis.bv_size);
  z3::expr_vector new_head_args(ctx);
  for (unsigned j = 0; j < head.num_args(); ++j) {
    z3::expr arg = head.arg(j);
    z3::expr translated_arg = translator.translate(arg);
    new_head_args.push_back(translated_arg);
  }
  new_head = p_head_new(new_head_args);
  z3::expr new_rule = z3::forall(new_vars, z3::implies(new_body, new_head));
  MOSAIC_DEBUG_MSG(OUT() << "Translated BV (size " << clause_analysis.bv_size
                         << ") CHC rule to int CHC rule: " << new_rule
                         << std::endl);
  return new_rule;
}

/// This function tries to find the correct refutation leaf predicate.
/// Example:
///     true -> q(x)
///     q(x) && phi -> r(x)
/// When analyzing the refutation leaf, spacer might inline the first clause
/// and our analysis would see the refutation leaf as r(x) instead of q(x).
/// To fix this, we solve backwards from r(x) through the clause, to get the
/// values of the variables in q(x).
static z3::expr evaluate_backwards(z3::expr const &q, z3::expr const &clause) {
  MOSAIC_DEBUG_MSG(OUT() << "Evaluating backwards query: " << q
                         << "\nthrough clause:\n"
                         << clause << std::endl);
  z3::expr clause_body = get_clause_body(clause, /*is_query*/ false);
  z3::expr clause_head = get_clause_head(clause, /*is_query*/ false);
  z3::expr_vector body_conjuncts = utils::get_conjuncts(clause_body);

  z3::context &ctx = clause.ctx();
  z3::expr body_pred(ctx);
  z3::expr_vector non_pred_conjuncts(ctx);
  for (const z3::expr &body_conjunct : body_conjuncts) {
    if (utils::is_uninterpreted_predicate(body_conjunct)) {
      assert(!bool(body_pred) && "We don't currently support non linear CHCs");
      body_pred = body_conjunct;
    } else {
      non_pred_conjuncts.push_back(body_conjunct);
    }
  }
  z3::expr body_no_pred = z3::mk_and(non_pred_conjuncts);
  assert(z3::eq(clause_head.decl(), q.decl()) &&
         "Clause head predicate does not match query predicate");

  z3::expr_vector head_arg_expr_assignment(ctx);
  for (unsigned i = 0; i < clause_head.num_args(); ++i) {
    head_arg_expr_assignment.push_back(q.arg(i) == clause_head.arg(i));
  }
  // Add the head argument assignments to the body without predicate
  body_no_pred = body_no_pred && z3::mk_and(head_arg_expr_assignment);

  VarMap unknown_var_subs;
  z3::expr eval_body_no_pred =
      evaluate_clause_vars(body_no_pred, unknown_var_subs);

  z3::solver solver(ctx);
  solver.add(eval_body_no_pred);
  z3::check_result res = solver.check();
  assert(res == z3::sat && "Could not evaluate body without predicate");
  z3::model m = solver.get_model();

  z3::expr new_q(ctx);
  z3::expr_vector new_q_args(ctx);
  for (unsigned i = 0; i < body_pred.num_args(); ++i) {
    z3::expr arg = body_pred.arg(i);
    assert(unknown_var_subs.find(arg) != unknown_var_subs.end() &&
           "Argument not found in unknown var substitutions");
    z3::expr unknown_var = unknown_var_subs.at(arg);
    z3::expr val = m.eval(unknown_var, /*model_completion*/ true);
    new_q_args.push_back(val);
  }

  new_q = body_pred.decl()(new_q_args);
  return new_q;
}

z3::expr MosaicFixedpoint::get_bv_expr_bounds(z3::expr_vector const &vars,
                                              unsigned bv_size) const {
  bool is_signed = m_is_signed.value();

  int64_t N = (int64_t)1 << bv_size;
  int64_t N_half = (int64_t)1 << (bv_size - 1);
  z3::expr_vector bound_lemmas(m_ctx);
  z3::expr tmp(m_ctx);
  // TODO: Consider being consistent with the rest of project and use <=
  for (const z3::expr &var : vars) {
    assert(var.is_int());
    if (is_signed)
      tmp = (var >= m_ctx.int_val(-N_half)) && (var < m_ctx.int_val(N_half));
    else
      tmp = (var >= m_ctx.int_val(0)) && (var < m_ctx.int_val(N));
    bound_lemmas.push_back(tmp);
  }

  return z3::mk_and(bound_lemmas);
}

z3::expr
MosaicFixedpoint::get_refutation_leaf_pred(z3::expr const &refutation) const {
  // The refutation is a modus ponens tree
  // The first argument is the hyper-resolution predicate
  z3::expr hyper_res_pred = refutation.arg(0);

  // Take the query predicate
  z3::expr q_pred = hyper_res_pred.arg(1);

  // If the query predicate is a hyper-resolution predicate, we need to find the
  // leaf
  while (is_hyper_res(q_pred)) {
    q_pred = q_pred.arg(1);
  }

  return q_pred;
}

z3::expr
MosaicFixedpoint::get_refutation_leaf_phi(z3::expr const &q,
                                          z3::expr_vector const &vars) const {
  assert(vars.size() <= q.num_args() &&
         "Predicate has too much arity for the given variables");
  z3::expr_vector conjuncts(m_ctx);
  for (unsigned i = 0; i < vars.size(); ++i) {
    //! IMPORTANT NOTE
    // This works under the assumption that the order
    // of variables in vars corresponds to the order
    // of arguments in q.
    // This could happen when the query predicate has 2 arguments
    // but the refutation creates q!X with the base arguments plus
    // variables that are defined in the clause.
    conjuncts.push_back(vars[i] == q.arg(i));
  }
  return z3::mk_and(conjuncts);
}

std::string MosaicFixedpoint::get_fresh_added_fact_name() {
  return kAdded_fact_name + std::to_string(added_fact_counter++);
}

void MosaicFixedpoint::check_signedness_consistency(
    ClauseAnalysisResult const &clause_analysis) {
  if (clause_analysis.has_conflicting_signedness())
    MOSAIC_ASSERT_FALSE("Clause has conflicting signedness information");

  if (clause_analysis.is_signedness_determined()) {
    bool clause_is_signed = clause_analysis.get_is_signed();
    if (!m_mth_fp_set.check_and_set_signedness(clause_is_signed)) {
      MOSAIC_ASSERT_FALSE("Conflicting signedness information between clauses");
    }
  }
}

z3::expr MosaicFixedpoint::populate_MTH_fixedpoint_engines(
    z3::expr const &query, ClauseAnalysisResult const &query_analysis) {
  // After making sure that signedness is consistent within each clause,
  // and between clauses, we make a final check to see if the signedness
  // is determined ("known") globally. If not, we cannot proceed.
  std::optional<bool> global_signedness = m_mth_fp_set.get_global_signedness();
  if (!global_signedness.has_value())
    MOSAIC_ASSERT_FALSE(
        "Could not determine global signedness from the clauses");

  bool is_signed = global_signedness.value();
  for (auto clause : m_original_clauses) {
    z3::symbol name = m_mth_fp_set.get_fresh_rule_name();
    auto clause_analysis = m_clause_analysis_map.at(clause);

    if (clause_analysis.has_bit_manipulating_apps ||
        !clause_analysis.is_bv_size_determined()) {
      z3::func_decl clause_head_pred =
          get_clause_head(clause, /*is_query*/ false).decl();
      m_mth_fp_set.getOrInitBVSolver().fp_solver.register_relation(
          clause_head_pred);
      m_mth_fp_set.getOrInitBVSolver().fp_solver.add_rule(clause, name);
      m_mth_fp_set.populate_predicate_maps_for_clause(
          clause, &m_mth_fp_set.getBVSolver(), /*is_query*/ false);
      m_mth_fp_set.map_clause_to_solver(clause, &m_mth_fp_set.getBVSolver());
    } else {
      // Bv size is determined and there are no bit-manipulating apps
      unsigned bv_size = clause_analysis.bv_size;
      z3::expr_vector bv_vars_exprs = get_clause_vars(clause);
      VarMap bv_to_int_var_map =
          build_bv2int_var_map(m_ctx, bv_vars_exprs, clause_analysis);
      // Initialize a translator with no_overflow guarantee
      Bv2IntTranslator translator(m_ctx, is_signed, /*simplify*/ true,
                                  /*no_overflow*/ true, bv_to_int_var_map);
      z3::expr int_clause = translate_BV_CHC_rule(
          clause, clause_analysis, translator, /*is_query*/ false);
      z3::func_decl int_clause_head_pred =
          get_clause_head(int_clause, /*is_query*/ false).decl();
      m_mth_fp_set.getOrInitIAUFSolver(bv_size).fp_solver.register_relation(
          int_clause_head_pred);
      m_mth_fp_set.getOrInitIAUFSolver(bv_size).fp_solver.add_rule(int_clause,
                                                                   name);
      m_mth_fp_set.populate_predicate_maps_for_clause(
          int_clause, &m_mth_fp_set.getIAUFSolver(bv_size), /*is_query*/ false);
      m_mth_fp_set.map_clause_to_solver(int_clause,
                                        &m_mth_fp_set.getIAUFSolver(bv_size));
    }
  }

  // Finally, add the query to the appropriate fixedpoint engine
  if (query_analysis.has_bit_manipulating_apps ||
      !query_analysis.is_bv_size_determined()) {
    m_mth_fp_set.getOrInitBVSolver().query = query;
    m_mth_fp_set.populate_predicate_maps_for_clause(
        query, &m_mth_fp_set.getBVSolver(), /*is_query*/ true);
    m_mth_fp_set.map_clause_to_solver(query, &m_mth_fp_set.getBVSolver());
  } else {
    // Bv size is determined and there are no bit-manipulating apps
    unsigned bv_size = query_analysis.bv_size;
    z3::expr_vector bv_vars_exprs = get_clause_vars(query);
    VarMap bv_to_int_var_map =
        build_bv2int_var_map(m_ctx, bv_vars_exprs, query_analysis);
    // Initialize a translator with no_overflow guarantee
    Bv2IntTranslator translator(m_ctx, is_signed, /*simplify*/ true,
                                /*no_overflow*/ true, bv_to_int_var_map);
    z3::expr int_query = translate_BV_CHC_rule(query, query_analysis,
                                               translator, /*is_query*/ true);
    m_mth_fp_set.getOrInitIAUFSolver(bv_size).query = int_query;
    m_mth_fp_set.populate_predicate_maps_for_clause(
        int_query, &m_mth_fp_set.getIAUFSolver(bv_size), /*is_query*/ true);
    m_mth_fp_set.map_clause_to_solver(int_query,
                                      &m_mth_fp_set.getIAUFSolver(bv_size));
    return int_query;
  }

  return query;
}

void MosaicFixedpoint::add_predicate_fact(z3::expr const &src_expr,
                                          z3::expr const &dst_expr,
                                          MTHSolver *dst_fp) {
  // Create fresh variables according to dst_expr's sort
  assert(src_expr.num_args() == dst_expr.num_args() &&
         "Source and destination predicates must have the same arity");
  z3::expr_vector vars(m_ctx);
  std::string dst_decl_name = dst_expr.decl().name().str();
  for (unsigned i = 0; i < dst_expr.num_args(); ++i) {
    z3::sort arg_sort = dst_expr.arg(i).get_sort();
    std::string var_name = dst_decl_name + "_fact_var_" + std::to_string(i);
    z3::expr var = m_ctx.constant(var_name.c_str(), arg_sort);
    vars.push_back(var);
  }

  // Store the newly created dst variables to be used later
  m_interface_dst_vars.emplace(src_expr.decl(), vars);

  z3::expr dst_new = dst_expr.decl()(vars);
  std::string fact_name_str = get_fresh_added_fact_name();
  z3::symbol fact_name = m_ctx.str_symbol(fact_name_str.c_str());
  z3::expr bound_expr = m_ctx.bool_val(true);
  if (!dst_fp->is_bv_solver())
    bound_expr = get_bv_expr_bounds(vars, dst_fp->get_bv_size());
  CHC fact(vars, m_ctx.bool_val(true), bound_expr, dst_new);
  z3::expr added_fact(m_ctx);
  added_fact = fact.get_rule_expr();
  z3::func_decl dst_decl = dst_expr.decl();
  dst_fp->fp_solver.register_relation(dst_decl);
  dst_fp->fp_solver.add_rule(added_fact, fact_name);

  // Store the fact in the fact map with the head predicate decl AST as the key
  m_interface_dst_fact_map.emplace(src_expr.decl(),
                                   std::make_pair(fact, fact_name));
}

void MosaicFixedpoint::add_interface_constraint(z3::expr p1_expr,
                                                z3::expr p2_expr,
                                                MTHSolver *fp2) {
  if (!m_interface_constraint_map.insert(p1_expr, p2_expr))
    return;
  MOSAIC_DEBUG_MSG(OUT() << "Adding interface constraint: " << p1_expr
                         << " ------> " << p2_expr << std::endl);
  m_interface_src_strengthening_map.emplace(p1_expr.decl(),
                                            m_ctx.bool_val(true));
  add_predicate_fact(p1_expr, p2_expr, fp2);
}

void MosaicFixedpoint::generate_interface_constraints() {
  if (!m_mth_fp_set.hasBVSolver())
    return;

  // Go over all IAUF solvers and their rules
  for (auto &[bv_size, iauf_solver] : m_mth_fp_set.getIAUFSolvers()) {
    for (const auto &clause : iauf_solver.get_all_clauses()) {
      std::optional<z3::expr> clause_head_pred_or_fail =
          m_mth_fp_set.get_clause_head_pred(clause);
      std::optional<z3::expr_vector> clause_body_preds_or_fail =
          m_mth_fp_set.get_clause_body_preds(clause);
      assert(clause_head_pred_or_fail.has_value() ==
                 clause_body_preds_or_fail.has_value() &&
             "Inconsistent presence of int clause head and body predicates");
      if (!clause_head_pred_or_fail && !clause_body_preds_or_fail) {
        // It means the clause is freshly added clause.
        continue;
      }
      z3::expr clause_head_pred = clause_head_pred_or_fail.value();
      z3::expr_vector clause_body_preds = clause_body_preds_or_fail.value();

      for (const auto &bv_clause :
           m_mth_fp_set.getBVSolver().get_all_clauses()) {
        std::optional<z3::expr> bv_clause_head_pred_or_fail =
            m_mth_fp_set.get_clause_head_pred(bv_clause);
        std::optional<z3::expr_vector> bv_clause_body_preds_or_fail =
            m_mth_fp_set.get_clause_body_preds(bv_clause);
        assert(bv_clause_head_pred_or_fail.has_value() ==
                   bv_clause_body_preds_or_fail.has_value() &&
               "Inconsistent presence of bv clause head and body predicates");
        if (!bv_clause_head_pred_or_fail && !bv_clause_body_preds_or_fail) {
          // It means the clause is freshly added clause.
          continue;
        }
        z3::expr bv_clause_head_pred = bv_clause_head_pred_or_fail.value();
        z3::expr_vector bv_clause_body_preds =
            bv_clause_body_preds_or_fail.value();

        // Check for BV -> INT interface constraints
        if (bool(bv_clause_head_pred) && !clause_body_preds.empty()) {
          std::string bv_pred_head_name =
              bv_clause_head_pred.decl().name().str();
          for (const z3::expr &body_pred : clause_body_preds) {
            std::string body_pred_name =
                extract_old_bv_predicate_name(body_pred.decl(), bv_size);
            if (bv_pred_head_name == body_pred_name) {
              // Found a BV -> INT interface constraint
              add_interface_constraint(bv_clause_head_pred, body_pred,
                                       &iauf_solver);
              if (bool(clause_head_pred))
                m_interface_dst_orig_head_to_clause_map.emplace(
                    clause_head_pred.decl(), clause);
            }
          }
        }

        // Check for INT -> BV interface constraints
        if (bool(clause_head_pred) && !bv_clause_body_preds.empty()) {
          std::string int_pred_head_name =
              extract_old_bv_predicate_name(clause_head_pred.decl(), bv_size);
          for (const z3::expr &bv_body_pred : bv_clause_body_preds) {
            std::string bv_body_pred_name = bv_body_pred.decl().name().str();
            if (int_pred_head_name == bv_body_pred_name) {
              // Found an INT -> BV interface constraint
              add_interface_constraint(clause_head_pred, bv_body_pred,
                                       &m_mth_fp_set.getBVSolver());
              if (bool(bv_clause_head_pred))
                m_interface_dst_orig_head_to_clause_map.emplace(
                    bv_clause_head_pred.decl(), bv_clause);
            }
          }
        }
      }
    }
  }
}

void MosaicFixedpoint::set_params_for_all_engines() {
  z3::params params =
      (m_params_set) ? m_params : get_default_mth_fp_params(m_ctx);
  MOSAIC_DEBUG_MSG(OUT() << "Setting parameters for all fixedpoint engines:\n"
                         << params << std::endl);
  if (m_mth_fp_set.hasBVSolver())
    m_mth_fp_set.getBVSolver().fp_solver.set(params);

  for (auto &[bv_size, iauf_solver] : m_mth_fp_set.getIAUFSolvers())
    iauf_solver.fp_solver.set(params);
}

//==============================================================================
//                              PUBLIC METHODS
//==============================================================================
MosaicFixedpoint::MosaicFixedpoint(z3::context &ctx, bool force_preprocess,
                                   bool simplify)
    : m_ctx(ctx), m_simplify(simplify), m_int2bv_preprocess(force_preprocess),
      m_mth_fp_set(ctx), m_params_set(false), m_params(ctx),
      m_original_clauses(ctx) {}

void MosaicFixedpoint::from_solver(z3::fixedpoint &fp) {
  MOSAIC_DEBUG_MSG(OUT() << "Importing rules from fixedpoint solver:\n"
                         << fp << std::endl);
  assert(fp.assertions().empty() && "Assertions are not supported yet");
  m_original_clauses = fp.rules();

  // The first loop analyzes the clauses to determine the global signedness
  // which is needed before we do anything else.
  // Also, to do the analysis only once, we store the analysis results in a map.
  for (auto clause : m_original_clauses) {
    MOSAIC_DEBUG_MSG(OUT() << "Analyzing clause:\n" << clause << std::endl);
    ClauseAnalysisResult clause_analysis = analyze_clause(m_ctx, clause);
    MOSAIC_DEBUG_MSG(OUT() << clause_analysis);
    check_signedness_consistency(clause_analysis);
    m_clause_analysis_map.emplace(clause, clause_analysis);
  }
}

void MosaicFixedpoint::add_rule(z3::expr &rule, z3::symbol const &name) {
  MOSAIC_DEBUG_MSG(OUT() << "Analyzing clause:\n" << rule << std::endl);
  ClauseAnalysisResult clause_analysis = analyze_clause(m_ctx, rule);
  MOSAIC_DEBUG_MSG(OUT() << clause_analysis);
  check_signedness_consistency(clause_analysis);
  m_clause_analysis_map.emplace(rule, clause_analysis);
  m_original_clauses.push_back(rule);
}

void MosaicFixedpoint::register_relation(z3::func_decl &p) {
  // Empty implementation.
  // The relations are registered right before the query.
  return;
}

void MosaicFixedpoint::set(z3::params const &p) {
  m_params = p;
  m_params_set = true;
}

z3::check_result MosaicFixedpoint::query(z3::expr &query) {
  MOSAIC_DEBUG_MSG(OUT() << "Analyzing query:\n" << query << std::endl);
  ClauseAnalysisResult query_analysis = analyze_clause(m_ctx, query);
  MOSAIC_DEBUG_MSG(OUT() << query_analysis);
  // Check signedness consistency and set global signedness
  check_signedness_consistency(query_analysis);
  m_is_signed = m_mth_fp_set.get_global_signedness();
  bool is_signed = m_is_signed.value();

  query = populate_MTH_fixedpoint_engines(query, query_analysis);
  MOSAIC_DEBUG_MSG(OUT() << "The set of solvers after population:\n"
                         << m_mth_fp_set << std::endl);

  // Generate interface constraints
  generate_interface_constraints();
  set_params_for_all_engines();
  MOSAIC_DEBUG_MSG(
      OUT() << "The set of solvers after interface constraint generation:\n"
            << m_mth_fp_set << std::endl);

  struct QueryConfig {
    z3::expr query;
    // If in the future we want to supprt non-linear CHCs,
    // we need to change p to be a vector of expressions.
    z3::expr p;
    MTHSolver *solver;
    QueryConfig(z3::expr &_query, z3::expr &_p, MTHSolver *_solver)
        : query(_query), p(_p), solver(_solver) {}
  };

  // Get first query id
  MTHSolver *query_solver = m_mth_fp_set.get_clause_solver(query);
  auto query_preds_or_fail = m_mth_fp_set.get_clause_body_preds(query);
  assert(query_preds_or_fail.has_value() && "Query body predicates not found");
  z3::expr_vector query_body_preds = query_preds_or_fail.value();
  assert(query_body_preds.size() == 1 &&
         "Expected exactly one body predicate in the query");
  z3::expr query_pred_expr = query_body_preds[0];

  // Create a query stack
  std::stack<QueryConfig> S;
  S.push(QueryConfig(query, query_pred_expr, query_solver));

  while (!S.empty()) {
    QueryConfig config = S.top();
    z3::expr current_query = config.query;
    MTHSolver *current_solver = config.solver;
    z3::expr p_expr = config.p;
    z3::func_decl p_decl = p_expr.decl();

    // Get the appropriate solver for the current query
    bool is_bv_solver = current_solver->is_bv_solver();
    MOSAIC_DEBUG_MSG(OUT() << "Querying " << (is_bv_solver ? "BV" : "Int")
                           << " engine:\n"
                           << current_solver->fp_solver << std::endl
                           << "With:\n"
                           << current_query << std::endl);

    // Query the solver
    z3::check_result res = current_solver->fp_solver.query(current_query);
    MOSAIC_DEBUG_MSG(OUT() << "Result: " << res << std::endl);

    if (res == z3::unsat) {
      S.pop();
      if (!S.empty()) {
        z3::expr p_interp =
            current_solver->fp_solver.get_cover_delta(-1, p_decl);
        MOSAIC_DEBUG_MSG(OUT() << "Interpretation of " << p_decl.name() << ":\n"
                               << p_interp << std::endl);
        p_interp = p_interp.substitute(utils::get_args(p_expr));
        MOSAIC_DEBUG_MSG(OUT() << "Substituted interpretation of "
                               << p_decl.name() << ":\n"
                               << p_interp << std::endl);

        VarMap var_map;
        z3::expr_vector dst_fact_vars = m_interface_dst_vars.at(p_decl);
        for (unsigned i = 0; i < p_expr.num_args(); ++i) {
          z3::expr src_var = p_expr.arg(i);
          z3::expr dst_var = dst_fact_vars[i];
          var_map.emplace(src_var, dst_var);
        }

        z3::expr p_interp_trans(m_ctx);
        if (is_bv_solver) {
          Bv2IntTranslator bv2int_t(m_ctx, is_signed, m_simplify,
                                    /* no_overflow */ false, var_map);
          p_interp_trans = bv2int_t.translate(p_interp);
        } else {
          unsigned bv_size = current_solver->get_bv_size();
          Int2BvTranslator int2bv_t(m_ctx, is_signed /*is_signed*/, bv_size,
                                    /*force_preprocess*/ m_int2bv_preprocess,
                                    m_simplify, var_map);
          p_interp_trans = int2bv_t.translate(p_interp);
        }
        MOSAIC_DEBUG_MSG(OUT() << "Translated interpretation of "
                               << p_decl.name() << ":\n"
                               << p_interp_trans << std::endl);

        // Strengthen the strengthening expression
        auto st_it = m_interface_src_strengthening_map.find(p_decl);
        assert(st_it != m_interface_src_strengthening_map.end());
        if (!p_interp.is_true())
          st_it->second = st_it->second && p_interp;

        //! IMPORTANT NOTE
        // We assume at most one fact is registered for each predicate
        auto it = m_interface_dst_fact_map.find(p_decl);
        assert(it != m_interface_dst_fact_map.end() &&
               "Fact not found in the map");
        CHCFactConfig *fact_config = &it->second;   // get the config
        z3::symbol fact_name = fact_config->second; // get CHC fact name
        MOSAIC_DEBUG_MSG(OUT() << "Fact name: " << fact_name << std::endl);
        MOSAIC_DEBUG_MSG(OUT()
                         << "Old fact: " << fact_config->first.get_rule_expr()
                         << std::endl);

        // Strengthen the fact
        if (!p_interp.is_true())
          fact_config->first.body_formula =
              fact_config->first.body_formula && p_interp_trans;
        z3::expr new_fact = fact_config->first.get_rule_expr();
        MOSAIC_DEBUG_MSG(OUT() << "New fact: " << new_fact << std::endl);

        // Update the fact in the appropriate fixedpoint engine
        z3::expr fact_head = fact_config->first.head;
        MTHSolver *fact_solver =
            m_mth_fp_set.get_predicate_solver(fact_head.decl());
        assert(fact_solver && "Fact solver not found");
        fact_solver->fp_solver.update_rule(new_fact, fact_name);
      }
    } else if (res == z3::sat) {
      z3::expr g_refutation = current_solver->fp_solver.get_answer();
      MOSAIC_DEBUG_MSG(OUT() << "Refutation:\n" << g_refutation << std::endl);
      // Extract the refutation leaf predicate
      z3::expr q = get_refutation_leaf_pred(g_refutation);
      MOSAIC_DEBUG_MSG(OUT()
                       << "Refutation leaf predicate: " << q << std::endl);
      // If we're dealing with a non user-visible predicate, use the query
      // predicate from the config, otherwise use the query predicate from the
      // refutation
      bool is_user_visible = is_user_visible_predicate(q.decl());
      z3::func_decl search_decl = (is_user_visible) ? q.decl() : p_decl;
      auto q_tag = m_interface_constraint_map.find_pred(search_decl);

      // We only do one backwards evaluation step
      if (!q_tag && is_user_visible &&
          m_interface_dst_orig_head_to_clause_map.find(search_decl) !=
              m_interface_dst_orig_head_to_clause_map.end()) {
        z3::expr dst_orig_clause =
            m_interface_dst_orig_head_to_clause_map.at(search_decl);
        q = evaluate_backwards(q, dst_orig_clause);
        MOSAIC_DEBUG_MSG(
            OUT()
            << "After evaluating backwards, new refutation leaf predicate: "
            << q << std::endl);
        search_decl = q.decl();
      }

      q_tag = m_interface_constraint_map.find_pred(search_decl);
      if (q_tag) {
        z3::expr_vector dst_vars =
            m_interface_dst_vars.at(q_tag.value().decl());
        z3::expr phi = get_refutation_leaf_phi(q, dst_vars);
        MOSAIC_DEBUG_MSG(OUT() << "phi: " << phi << std::endl);

        z3::expr phi_trans(m_ctx);
        z3::expr_vector translated_vars(m_ctx);
        if (is_bv_solver) {
          Bv2IntTranslator bv2int_t(m_ctx, is_signed, m_simplify);
          phi_trans = bv2int_t.translate(phi);
          // Go over all the lemmas and conjoin them with the translated
          // predicate
          z3::expr_vector lemmas(m_ctx);
          for (const auto &kv : bv2int_t.lemmas()) {
            lemmas.push_back(kv.second);
          }
          phi_trans = (phi_trans) && z3::mk_and(lemmas);
          translated_vars = bv2int_t.vars();
        } else {
          unsigned bv_size = current_solver->get_bv_size();
          Int2BvTranslator int2bv_t(m_ctx, is_signed /*is_signed*/, bv_size,
                                    /*force_preprocess*/ m_int2bv_preprocess,
                                    m_simplify);
          phi_trans = int2bv_t.translate(phi);
          translated_vars = int2bv_t.vars();
        }

        MOSAIC_DEBUG_MSG(OUT() << "Translated phi: " << phi_trans << std::endl);
        z3::expr q_tag_new = q_tag.value().decl()(translated_vars);
        MOSAIC_DEBUG_MSG(OUT() << "q' (with translated vars) = " << q_tag_new
                               << std::endl);

        auto st_it =
            m_interface_src_strengthening_map.find(q_tag.value().decl());
        assert(st_it != m_interface_src_strengthening_map.end() &&
               "Strengthening expression not found");
        z3::expr query_phi = phi_trans || !(st_it->second);

        z3::expr next_query =
            z3::exists(translated_vars, q_tag_new && query_phi);
        MTHSolver *next_solver =
            m_mth_fp_set.get_predicate_solver(q_tag.value().decl());
        S.push(QueryConfig(next_query, q_tag_new, next_solver));
      } else {
        return z3::sat;
      }
    } else {
      return z3::unknown;
    }
  }

  return z3::unsat;
}
} // namespace lotus::mosaic
