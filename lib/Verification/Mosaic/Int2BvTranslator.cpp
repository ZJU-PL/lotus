// Int2BvTranslator.cpp

#include "Verification/Mosaic/Int2BvTranslator.h"

#define LDBG(x) MOSAIC_DEBUG_MSG(OUT() << "[Int2BvTranslator] " << x)

namespace lotus::mosaic {

Int2BvTranslator::Int2BvTranslator(z3::context &c, bool is_signed,
                                   unsigned bv_size, bool force_preprocess,
                                   bool simplify, const VarMap &int2bv_var_map)
    : ctx(c), m_bv_size(bv_size), m_is_signed(is_signed), m_simplify(simplify),
      m_force_preprocess(force_preprocess), m_int2bv_var_map(int2bv_var_map),
      m_extended_bv_size(bv_size), m_is_vars_signed(is_signed), m_vars(c) {}

void Int2BvTranslator::reset() { m_translate.clear(); }

bool Int2BvTranslator::is_special_basic(const z3::expr &e) const {
  return is_basic(e) && (e.is_ite() || e.is_eq());
}

bool Int2BvTranslator::is_basic(const z3::expr &e) const {
  Z3_decl_kind f = e.decl().decl_kind();
  return (Z3_OP_TRUE <= f && f <= Z3_OP_OEQ);
}

bool Int2BvTranslator::is_int_relation(const z3::expr &e) const {
  Z3_decl_kind f = e.decl().decl_kind();
  bool has_int_arg =
      utils::any_of(utils::get_args(e),
                    [&](z3::expr arg) { return arg.is_int(); });
  return Z3_OP_LE <= f && f <= Z3_OP_GT && has_int_arg;
}

z3::expr Int2BvTranslator::translate_aux(const z3::expr &e) {
  Z3_ast key = e; // implicit cast to Z3_ast
  auto it = m_translate.find(key);
  // This condition is important, it prevents redundant work
  if (it != m_translate.end())
    return it->second;

  z3::expr r(ctx);
  if (e.is_quantifier()) {
    // In case of horn clauses, this shouldn't be reached
    // This should be unrecahable as quantifiers should not be present
    // in the CHC BV expressions.
    MOSAIC_UNREACHABLE();
  } else if (e.is_var()) {
    r = translate_const_variable(e);
  } else { // is_app
    if (e.is_const() && !e.is_numeral() && !e.is_bool()) {
      r = translate_const_variable(e);
    } else if (is_special_basic(e)) {
      r = translate_special_basic(e);
    } else if (is_basic(e)) {
      r = translate_basic(e);
    } else if (is_int_relation(e)) {
      r = translate_int(e);
    } else { // is_int
      // Translate arguments
      assert(e.is_int() && "Expected an Int expression");
      r = translate_int(e);
    }
  }

  // Simplify the result expression
  if (m_simplify)
    r = r.simplify();
  m_translate.emplace(key, r);
  return r;
}

z3::expr Int2BvTranslator::translate_int(const z3::expr &e) {
  // Get INT operation kind
  Z3_decl_kind f = e.decl().decl_kind();

  // Collect arguments
  std::vector<z3::expr> args;
  for (unsigned i = 0; i < e.num_args(); ++i)
    args.push_back(translate_aux(e.arg(i)));

  // Result expression
  z3::expr r(ctx);

  switch (f) {
  // Arithmetic
  case Z3_OP_ANUM:
    // Translate numeral to BV
    assert(e.is_numeral() && "Z3_OP_ANUM should only be used with numerals");
    r = ctx.bv_val(e.get_numeral_int64(), m_extended_bv_size);
    break;
  case Z3_OP_AGNUM:
  case Z3_OP_TO_REAL:
  case Z3_OP_TO_INT:
  case Z3_OP_IS_INT:
  case Z3_OP_DIV:
    MOSAIC_ASSERT_FALSE("We're not supposed to encounter real numbers");
    break;
  case Z3_OP_LE:
    r = (m_is_signed) ? args[0] <= args[1] : z3::ule(args[0], args[1]);
    break;
  case Z3_OP_GE:
    r = (m_is_signed) ? args[0] >= args[1] : z3::uge(args[0], args[1]);
    break;
  case Z3_OP_LT:
    r = (m_is_signed) ? args[0] < args[1] : z3::ult(args[0], args[1]);
    break;
  case Z3_OP_GT:
    r = (m_is_signed) ? args[0] > args[1] : z3::ugt(args[0], args[1]);
    break;
  case Z3_OP_ADD:
    r = args[0] + args[1];
    break;
  case Z3_OP_SUB:
    r = args[0] - args[1];
    break;
  case Z3_OP_UMINUS:
    r = -args[0];
    break;
  case Z3_OP_MUL:
    r = args[0] * args[1];
    break;
  case Z3_OP_IDIV:
    r = (m_is_signed) ? args[0] / args[1] : z3::udiv(args[0], args[1]);
    break;
  case Z3_OP_REM:
    r = (m_is_signed) ? srem(args[0], args[1]) : z3::urem(args[0], args[1]);
    break;
  case Z3_OP_MOD:
    r = (m_is_signed) ? smod(args[0], args[1]) : z3::urem(args[0], args[1]);
    break;
  case Z3_OP_POWER:
    MOSAIC_ASSERT_FALSE("NOT SUPPORTED through z3++.h");
    break;
  default:
    // fallback: treat as uninterpreted
    // OUT() << f << std::endl;
    MOSAIC_ASSERT_FALSE("Unsupported Int operation");
  }

  return r;
}

z3::expr Int2BvTranslator::translate_special_basic(const z3::expr &e) {
  assert(is_basic(e) && "Expected a basic expression (equality or ite)");
  if (e.is_ite()) {
    // ite is translated to ite
    z3::expr cond = translate_aux(e.arg(0));
    z3::expr then_branch = translate_aux(e.arg(1));
    z3::expr else_branch = translate_aux(e.arg(2));
    return ite(cond, then_branch, else_branch);
  } else {
    // Equality
    z3::expr lhs = translate_aux(e.arg(0));
    z3::expr rhs = translate_aux(e.arg(1));
    return lhs == rhs;
  }
  return e;
}

z3::expr Int2BvTranslator::translate_const_variable(const z3::expr &e) {
  // Note: numerals are handled in translate_bv: Z3_OP_ANUM
  // Constants are apps with no arguments
  z3::expr r(ctx);
  if (m_int2bv_var_map.find(e) != m_int2bv_var_map.end()) {
    // If we have a mapping for this constant use it
    r = m_int2bv_var_map.at(e);
  } else {
    // Otherwise, create a new BV constant
    std::string name;
    if (e.is_var())
      name = fresh_var_name + std::to_string(var_count++);
    else
      name = e.decl().name().str();
    r = ctx.bv_const(name.c_str(), m_bv_size);
  }

  m_vars.push_back(r);

  if (m_extended_bv_size > m_bv_size) {
    if (m_is_vars_signed) {
      // Sign-extend to the extended size
      r = z3::sext(r, m_extended_bv_size - m_bv_size);
    } else {
      // Zero-extend to the extended size
      r = z3::zext(r, m_extended_bv_size - m_bv_size);
    }
  }

  return r;
}

z3::expr Int2BvTranslator::translate_basic(const z3::expr &e) {
  assert(!is_special_basic(e) &&
         "Expected a basic expression (not ite or equality)");
  z3::expr_vector new_args(ctx);
  for (unsigned i = 0; i < e.num_args(); ++i) {
    new_args.push_back(translate_aux(e.arg(i)));
  }

  // Create a new application with the translated arguments
  return e.decl()(new_args);
}

void Int2BvTranslator::determine_extension_config(const z3::expr &e) {
  Int2BvOverflowAnalyzer oa(ctx);
  unsigned extended_size = 0;
  if (m_is_signed) {
    for (unsigned d = 0; d <= MAX_MTH_BV_SIZE - m_bv_size; ++d) {
      extended_size = m_bv_size + d;
      if (!oa.overflows(e, /*bv_size_vars*/ m_bv_size,
                        /*bv_size_funcs_consts*/ extended_size,
                        /*is_signed_vars*/ true,
                        /*is_signed_func_consts*/ true)) {
        break;
      }
      oa.reset();
    }
  } else {
    for (unsigned d = 0; d <= MAX_MTH_BV_SIZE - m_bv_size; ++d) {
      extended_size = m_bv_size + d;
      if (d == 0) {
        if (!oa.overflows(e, /*bv_size_vars*/ m_bv_size,
                          /*bv_size_funcs_consts*/ extended_size,
                          /*is_signed_vars*/ false,
                          /*is_signed_func_consts*/ false))
          break;
      } else if (!oa.overflows(e, /*bv_size_vars*/ m_bv_size,
                               /*bv_size_funcs_consts*/ extended_size,
                               /*is_signed_vars*/ false,
                               /*is_signed_func_consts*/ true)) {
        // Translate to signed bit-vectors when extended even though
        // originally unsigned
        m_is_signed = true;
        break;
      }
      oa.reset();
    }
  }

  assert(extended_size <= MAX_MTH_BV_SIZE &&
         "Exceeded maximum bit-vector size extension");
  m_extended_bv_size = extended_size;
}

z3::expr Int2BvTranslator::translate(const z3::expr &e, bool handle_overflow) {
  if (!handle_overflow)
    return translate_aux(e);

  Int2BvPreprocessor preprocessor(ctx, m_bv_size, m_is_signed);
  z3::expr expr_to_translate = e;
  if (m_force_preprocess) {
    expr_to_translate = preprocessor.preprocess(e);
    LDBG("Preprocessed expr: " << expr_to_translate << "\n");
  } else {
    // We try to avoid preprocessing here by translating to larger
    // bit-vectors only if necessary.
    // Set the extended bv size based on the signedness.
    determine_extension_config(e);
    LDBG("Determined extended BV size: " << m_extended_bv_size << "\n");
  }

  return translate_aux(expr_to_translate);
}
} // namespace lotus::mosaic
