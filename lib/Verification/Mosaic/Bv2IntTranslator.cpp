// Bv2IntTranslator.cpp

#include "Verification/Mosaic/Bv2IntTranslator.h"

#define LDBG(x) MOSAIC_DEBUG_MSG(OUT() << "[Bv2IntTranslator] " << x)
namespace lotus::mosaic {

Bv2IntTranslator::Bv2IntTranslator(z3::context &c, bool is_signed,
                                   bool simplify, bool no_overflow,
                                   const VarMap &bv2int_var_map)
    : ctx(c), m_is_signed(is_signed), m_simplify(simplify),
      m_no_overflow(no_overflow), m_vars(c), m_bv2int_var_map(bv2int_var_map) {}

void Bv2IntTranslator::reset() {
  m_translate.clear();
  m_int_var_bitwidth.clear();
  m_lemmas.clear();
}

bool Bv2IntTranslator::is_special_basic(const z3::expr &e) const {
  return is_basic(e) && (e.is_ite() || e.is_eq());
}

bool Bv2IntTranslator::is_basic(const z3::expr &e) const {
  Z3_decl_kind f = e.decl().decl_kind();
  return (Z3_OP_TRUE <= f && f <= Z3_OP_OEQ);
}

bool Bv2IntTranslator::is_casting(const z3::expr &e) const {
  Z3_decl_kind f = e.decl().decl_kind();
  return f == Z3_OP_BV2INT || f == Z3_OP_INT2BV;
}

bool Bv2IntTranslator::is_bv_relation(const z3::expr &e) const {
  Z3_decl_kind f = e.decl().decl_kind();
  bool has_bv_arg =
      utils::any_of(utils::get_args(e),
                    [&](z3::expr arg) { return arg.is_bv(); });
  return Z3_OP_ULEQ <= f && f <= Z3_OP_SGT && has_bv_arg;
}

bool Bv2IntTranslator::is_const_variable(const z3::expr &e) const {
  // Check if the expression is a variable (0-arity application)
  return e.is_const() && !e.is_numeral() && !e.is_bool();
}

z3::expr Bv2IntTranslator::translate(const z3::expr &e) {
  if (m_no_overflow)
    return translate_no_overflow(e);
  return translate_overflow(e);
}

z3::expr Bv2IntTranslator::translate_no_overflow(const z3::expr &e) {
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
  } else {
    // is_app
    if (is_const_variable(e)) {
      r = translate_const_variable(e);
    } else if (is_basic(e)) {
      r = translate_basic(e);
    } else {
      // is bv
      r = translate_bv_no_overflow(e);
    }
  }

  // Simplify the result expression
  if (m_simplify)
    r = r.simplify();
  m_translate.emplace(key, r);
  return r;
}

z3::expr Bv2IntTranslator::translate_overflow(const z3::expr &e) {
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
    if (is_const_variable(e)) {
      r = translate_const_variable(e);
    } else if (is_special_basic(e)) {
      // Handle special basic cases: ite and eq
      r = translate_special_basic(e);
    } else if (is_basic(e)) {
      // && || implies iff, etc..
      assert(!is_special_basic(e) &&
             "Expected a basic expression (not ite or equality)");
      r = translate_basic(e);
    } else if (is_bv_relation(e)) {
      assert(e.is_bool() && "Expected a BV relation expression");
      r = translate_bv_rel(e);
    } else if (is_casting(e)) {
      r = translate_cast(e);
    } else { // is_bv
      r = translate_bv(e);
    }
  }

  // Simplify the result expression
  if (m_simplify)
    r = r.simplify();
  m_translate.emplace(key, r);
  return r;
}

z3::expr Bv2IntTranslator::translate_bv_no_overflow(const z3::expr &e) {
  // Get BV operation kind
  Z3_decl_kind f = e.decl().decl_kind();

  // Collect arguments
  std::vector<z3::expr> args;
  for (unsigned i = 0; i < e.num_args(); ++i)
    args.push_back(translate(e.arg(i)));

  // Result expression
  z3::expr r(ctx);
  unsigned k;
  uint64_t N;
  switch (f) {
  case Z3_OP_BNUM: {
    assert(e.is_numeral() && "Z3_OP_BNUM should only be used with numerals");
    uint64_t raw = e.get_numeral_uint64();
    if (m_is_signed) {
      int64_t raw_int = utils::sign_extend(raw, e.get_sort().bv_size());
      return ctx.int_val(raw_int);
    }
    return ctx.int_val(raw);
  }
  // Relational operations
  case Z3_OP_SLEQ:
  case Z3_OP_ULEQ:
    r = args[0] <= args[1];
    break;
  case Z3_OP_SGEQ:
  case Z3_OP_UGEQ:
    r = args[0] >= args[1];
    break;
  case Z3_OP_SLT:
  case Z3_OP_ULT:
    r = args[0] < args[1];
    break;
  case Z3_OP_SGT:
  case Z3_OP_UGT:
    r = args[0] > args[1];
    break;
  // Arithmetic operations
  case Z3_OP_BNEG:
    r = -args[0];
    break;
  case Z3_OP_BADD:
    r = args[0] + args[1];
    break;
  case Z3_OP_BSUB:
    r = args[0] - args[1];
    break;
  case Z3_OP_BMUL:
    r = args[0] * args[1];
    break;

  case Z3_OP_BSDIV:
  case Z3_OP_BSDIV_I:
    r = if_eq(args[1], 0, ctx.int_val(-1), args[0] / args[1]);
    break;
  case Z3_OP_BUDIV:
  case Z3_OP_BUDIV_I:
    k = e.arg(1).get_sort().bv_size();
    N = (uint64_t)1 << k;
    r = if_eq(args[1], 0, ctx.int_val(N - 1), args[0] / args[1]);
    break;
  case Z3_OP_BSMOD:
  case Z3_OP_BSMOD_I:
  case Z3_OP_BUREM:
  case Z3_OP_BUREM_I:
    r = if_eq(args[1], 0, args[0], args[0] % args[1]);
    break;

  // Extra cases
  case Z3_OP_BSREM:
  case Z3_OP_BSREM_I:
    k = e.arg(1).get_sort().bv_size();
    r = if_eq(args[1], 0, args[0], rem(args[0], args[1]));
    break;

  default:
    MOSAIC_ASSERT_FALSE("Unsupported BV op with no overflow guarantee");
  }

  return r;
}

z3::expr Bv2IntTranslator::translate_bv(const z3::expr &e) {
  // Get BV operation kind
  Z3_decl_kind f = e.decl().decl_kind();

  // Collect arguments
  std::vector<z3::expr> args;
  for (unsigned i = 0; i < e.num_args(); ++i)
    args.push_back(translate(e.arg(i)));

  // Result expression
  z3::expr r(ctx);
  unsigned k;
  uint64_t N;
  switch (f) {
  case Z3_OP_BNUM: {
    assert(e.is_numeral() && "Z3_OP_BNUM should only be used with numerals");
    uint64_t raw = e.get_numeral_uint64();
    r = ctx.int_val(raw);
    break;
  }
  case Z3_OP_BNEG:
    k = e.arg(0).get_sort().bv_size();
    N = (uint64_t)1 << k;
    r = umod(ctx.int_val(N) - args[0], k);
    break;

  case Z3_OP_BADD:
    k = e.arg(1).get_sort().bv_size();
    N = (uint64_t)1 << k;
    r = umod(args[0] + args[1], k);
    break;
  case Z3_OP_BSUB:
    k = e.arg(1).get_sort().bv_size();
    N = (uint64_t)1 << k;
    r = umod(args[0] - args[1], k);
    break;
  case Z3_OP_BMUL:
    k = e.arg(1).get_sort().bv_size();
    N = (uint64_t)1 << k;
    r = umod(args[0] * args[1], k);
    break;

  case Z3_OP_BSDIV:
  case Z3_OP_BSDIV_I:
    k = e.arg(1).get_sort().bv_size();
    N = (uint64_t)1 << k;
    r = if_eq(args[1], 0, ctx.int_val(N - 1),
              stu(uts(args[0], k) / uts(args[1], k), k));
    break;
  case Z3_OP_BUDIV:
  case Z3_OP_BUDIV_I:
    k = e.arg(1).get_sort().bv_size();
    N = (uint64_t)1 << k;
    r = if_eq(args[1], 0, ctx.int_val(N - 1), args[0] / args[1]);
    break;
  case Z3_OP_BSREM:
  case Z3_OP_BSREM_I:
    k = e.arg(1).get_sort().bv_size();
    r = if_eq(args[1], 0, args[0],
              stu(rem(uts(args[0], k), uts(args[1], k)), k));
    break;
  case Z3_OP_BUREM:
  case Z3_OP_BUREM_I:
    k = e.arg(1).get_sort().bv_size();
    r = if_eq(args[1], 0, args[0], args[0] % args[1]);
    break;
  case Z3_OP_BSMOD:
  case Z3_OP_BSMOD_I:
    k = e.arg(1).get_sort().bv_size();
    r = if_eq(args[1], 0, args[0], stu(uts(args[0], k) % uts(args[1], k), k));
    break;

  case Z3_OP_BNOT:
    k = e.arg(1).get_sort().bv_size();
    N = (uint64_t)1 << k;
    r = ctx.int_val(N) - args[0] - ctx.int_val(1);
    break;
  case Z3_OP_BAND:
  case Z3_OP_BOR:
  case Z3_OP_BXOR:
  case Z3_OP_BNAND:
  case Z3_OP_BNOR:
  case Z3_OP_BXNOR:
    k = e.arg(1).get_sort().bv_size();
    r = create_bitwise_sum(f, args[0], args[1], k);
    break;

  case Z3_OP_CONCAT:
    k = e.arg(1).get_sort().bv_size();
    N = (uint64_t)1 << k;
    r = (args[0] * ctx.int_val(N)) + args[1];
    break;
  case Z3_OP_EXTRACT: {
    // Extract bits from a BV
    // (arg1 div 2^l) mod 2^(h-l+1)
    unsigned high = e.hi();
    unsigned low = e.lo();
    k = high - low + 1;
    uint64_t divL = (uint64_t)1 << (low);
    r = umod(args[0] / ctx.int_val(divL), high - low + 1);
    break;
  }
  case Z3_OP_BIT2BOOL: {
    unsigned bit_index = static_cast<unsigned>(
        Z3_get_decl_int_parameter(e.ctx(), e.decl(), 0));
    uint64_t divL = (uint64_t)1 << (bit_index);
    r = (umod(args[0] / ctx.int_val(divL), 1) == ctx.int_val(1));
    break;
  }
  case Z3_OP_ZERO_EXT: {
    // Zero extension doesn't change the underlying integer value
    r = args[0];
    break;
  }
  case Z3_OP_SIGN_EXT:
    // Implement only if needed
    MOSAIC_ASSERT_FALSE("Sign extension not implemented");
    break;

  case Z3_OP_REPEAT:
  case Z3_OP_BREDOR:
  case Z3_OP_BREDAND:
  case Z3_OP_BCOMP:
    MOSAIC_ASSERT_FALSE("Implement only if needed");
    break;

  case Z3_OP_BSHL:
    k = e.arg(1).get_sort().bv_size();
    r = umod(args[0] * pow2(args[1]), k);
    break;
  case Z3_OP_BLSHR:
    k = e.arg(1).get_sort().bv_size();
    r = args[0] / pow2(args[1]);
    break;
  case Z3_OP_BASHR:
  case Z3_OP_ROTATE_LEFT:
  case Z3_OP_ROTATE_RIGHT:
  case Z3_OP_EXT_ROTATE_LEFT:
  case Z3_OP_EXT_ROTATE_RIGHT:
    MOSAIC_ASSERT_FALSE("Implement only if needed");
    break;

  case Z3_OP_CARRY:
  case Z3_OP_XOR3:
    MOSAIC_ASSERT_FALSE("NOT SUPPORTED through z3++.h");
    break;
  case Z3_OP_BSMUL_NO_OVFL:
  case Z3_OP_BUMUL_NO_OVFL:
  case Z3_OP_BSMUL_NO_UDFL:
  default:
    // OUT() << f << std::endl;
    MOSAIC_ASSERT_FALSE("Unsupported BV operation");
  }

  return r;
}

z3::expr Bv2IntTranslator::translate_const_variable(const z3::expr &e) {
  // Note: numerals are handled in translate_bv: Z3_OP_BNUM
  // Constants are apps with no arguments
  z3::expr r(ctx);
  if (m_bv2int_var_map.find(e) != m_bv2int_var_map.end()) {
    // If we have a mapping for this constant, use it
    r = m_bv2int_var_map.at(e);
  } else {
    // Otherwise, create a new integer constant
    std::string name;
    if (e.is_var())
      name = fresh_var_name + std::to_string(var_count++);
    else
      name = e.decl().name().str();
    r = ctx.int_const(name.c_str());
  }

  // We only support constants (vars) of Bit-vector sort!
  assert(e.get_sort().is_bv() && "Expected a BV sort for constant");
  unsigned k = e.get_sort().bv_size();
  Z3_ast key = (Z3_ast)r;
  m_int_var_bitwidth.emplace(key, k);
  create_bound_lemma(r, k);
  m_vars.push_back(r);

  // We only perform stu if the translation is signed and overflow is possible
  if (!m_no_overflow && m_is_signed)
    r = stu(r, e.get_sort().bv_size());

  return r;
}

z3::expr Bv2IntTranslator::translate_cast(const z3::expr &e) {
  assert(is_casting(e) && "Expected a casting operation (bv2int or int2bv)");
  Z3_decl_kind f = e.decl().decl_kind();

  if (f == Z3_OP_BV2INT)
    MOSAIC_ASSERT_FALSE(
        "Bv2Int not implemented (unclear why this would even be needed)");

  // It's int2bv
  // Get the bv-size parameter
  unsigned bv_size = static_cast<unsigned>(
      Z3_get_decl_int_parameter(e.ctx(), e.decl(), 0));

  if (e.arg(0).is_numeral()) {
    return umod(e.arg(0), bv_size);
  } else if (is_basic(e.arg(0))) {
    // This is the way to turn a BV basic boolean operation into an Int
    return ite(translate_basic(e.arg(0)), ctx.int_val(1), ctx.int_val(0));
  } else if (is_bv_relation(e.arg(0))) {
    // This is the way to turn a BV relation into an Int
    return ite(translate_bv_rel(e.arg(0)), ctx.int_val(1), ctx.int_val(0));
  } else {
    // Otherwise, we don't know how to handle this case
    MOSAIC_ASSERT_FALSE("Unsupported argument for int2bv");
    return e;
  }
}

z3::expr Bv2IntTranslator::translate_bv_rel(const z3::expr &e) {
  Z3_decl_kind f = e.decl().decl_kind();
  unsigned k = e.arg(1).get_sort().bv_size();
  std::vector<z3::expr> args;
  for (unsigned i = 0; i < e.num_args(); ++i)
    args.push_back(translate(e.arg(i)));

  z3::expr r(ctx);
  switch (f) {
  case Z3_OP_ULEQ:
    r = args[0] <= args[1];
    break;
  case Z3_OP_SLEQ:
    r = uts(args[0], k) <= uts(args[1], k);
    break;
  case Z3_OP_UGEQ:
    r = args[0] >= args[1];
    break;
  case Z3_OP_SGEQ:
    r = uts(args[0], k) >= uts(args[1], k);
    break;
  case Z3_OP_ULT:
    r = args[0] < args[1];
    break;
  case Z3_OP_SLT:
    r = uts(args[0], k) < uts(args[1], k);
    break;
  case Z3_OP_UGT:
    r = args[0] > args[1];
    break;
  case Z3_OP_SGT:
    r = uts(args[0], k) > uts(args[1], k);
    break;
  default:
    MOSAIC_ASSERT_FALSE("Unsupported BV signed relation operation");
  }
  return r;
}

z3::expr Bv2IntTranslator::translate_special_basic(const z3::expr &e) {
  assert(is_basic(e) && "Expected a basic expression (equality or ite)");
  if (e.is_ite()) {
    // ite is translated to ite
    z3::expr cond = translate(e.arg(0));
    z3::expr then_branch = translate(e.arg(1));
    z3::expr else_branch = translate(e.arg(2));
    return ite(cond, then_branch, else_branch);
  } else {
    // Equality
    z3::expr lhs = translate(e.arg(0));
    z3::expr rhs = translate(e.arg(1));
    z3::expr res = simplify_equality_mod(lhs == rhs);
    return res;
  }
  return e;
}

z3::expr Bv2IntTranslator::translate_basic(const z3::expr &e) {
  z3::expr_vector new_args(ctx);
  for (unsigned i = 0; i < e.num_args(); ++i) {
    new_args.push_back(translate(e.arg(i)));
  }

  // Create a new application with the translated arguments
  return e.decl()(new_args);
}

z3::expr Bv2IntTranslator::create_bitwise_sum(const Z3_decl_kind &f,
                                              const z3::expr &arg1,
                                              const z3::expr &arg2,
                                              unsigned k) {
  // bit_tt(a,b) builds the "1/0" truth table ite expression
  std::function<z3::expr(z3::expr, z3::expr)> bit_tt;

  switch (f) {
  case Z3_OP_BAND:
    bit_tt = [&](z3::expr a, z3::expr b) {
      return ite(a == ctx.int_val(1) && b == ctx.int_val(1), ctx.int_val(1),
                 ctx.int_val(0));
    };
    break;
  case Z3_OP_BOR:
    bit_tt = [&](z3::expr a, z3::expr b) {
      return ite(a == ctx.int_val(1) || b == ctx.int_val(1), ctx.int_val(1),
                 ctx.int_val(0));
    };
    break;
  case Z3_OP_BXOR:
    bit_tt = [&](z3::expr a, z3::expr b) {
      return ite(a == ctx.int_val(1) ^ b == ctx.int_val(1), ctx.int_val(1),
                 ctx.int_val(0));
    };
    break;
  case Z3_OP_BNAND:
    bit_tt = [&](z3::expr a, z3::expr b) {
      return ite(a == ctx.int_val(1) && b == ctx.int_val(1), ctx.int_val(0),
                 ctx.int_val(1));
    };
    break;
  case Z3_OP_BNOR:
    bit_tt = [&](z3::expr a, z3::expr b) {
      return ite(a == ctx.int_val(1) || b == ctx.int_val(1), ctx.int_val(0),
                 ctx.int_val(1));
    };
    break;
  case Z3_OP_BXNOR:
    bit_tt = [&](z3::expr a, z3::expr b) {
      return ite(a == ctx.int_val(1) ^ b == ctx.int_val(1), ctx.int_val(0),
                 ctx.int_val(1));
    };
    break;
  default:
    MOSAIC_ASSERT_FALSE("unsupported bitwise op");
  }

  z3::expr sum = ctx.int_val(0);

  // per-bit lemmas
  for (unsigned i = 0; i < k; ++i) {
    z3::expr ai = bseli(arg1, i);
    z3::expr bi = bseli(arg2, i);
    sum = sum + pow2(ctx.int_val(i)) * bit_tt(ai, bi);
  }

  return sum;
}

void Bv2IntTranslator::create_bound_lemma(z3::expr &var, unsigned k) {
  // Create a lemma for the variable var, which is an Int variable
  // with bounds [0, 2^k - 1] if unsigned, or [-2^(k-1), 2^(k-1) - 1] if signed.
  // TODO: Consider being consistent with the rest of project and use <=
  if (m_is_signed) {
    int64_t N = (int64_t)1 << (k - 1);
    z3::expr lemma = (var >= ctx.int_val(-N)) && (var < ctx.int_val(N));
    m_lemmas.emplace(var, lemma);
    return;
  }

  int64_t N = (uint64_t)1 << k;
  z3::expr lemma = (var >= ctx.int_val(0)) && (var < ctx.int_val(N));
  m_lemmas.emplace(var, lemma);
}

z3::expr Bv2IntTranslator::bseli(const z3::expr &e, unsigned i) {
  // (e div 2^i) mod 2
  uint64_t N = (uint64_t)1 << i;
  return umod(e / ctx.int_val(N), 1);
}

z3::expr Bv2IntTranslator::umod(const z3::expr &e, unsigned k) {
  // unsigned modulo N = 2^k
  uint64_t N = (uint64_t)1 << k;
  return z3::mod(e, ctx.int_val(N));
}

z3::expr Bv2IntTranslator::uts(const z3::expr &e, unsigned k) {
  // 2 * umod(e, k - 1) - e
  return (ctx.int_val(2) * umod(e, k - 1)) - e;
}

z3::expr Bv2IntTranslator::stu(const z3::expr &e, unsigned k) {
  return umod(e, k);
}

z3::expr Bv2IntTranslator::pow2(const z3::expr &e) {
  // 2^e
  if (e.is_numeral()) {
    return ctx.int_val(1 << e.get_numeral_uint());
  }

  // Using z3::pw for exponentiation might produces real values because it's not
  // guaranteed that the exponent is a positive integer! This could cause
  // unepected behavior in case we don't handle reals properly.
  // TODO: Handle this issue in case it's needed.
  MOSAIC_ASSERT_FALSE("Expected a numeral for exponent in pow2. ");
  return e;
}

z3::expr Bv2IntTranslator::if_eq(const z3::expr &e, uint64_t k,
                                 const z3::expr &th, const z3::expr &el) {
  if (e.is_numeral()) {
    if (e.get_numeral_uint64() == k) {
      return th;
    } else {
      return el;
    }
  }
  return ite(e == ctx.int_val(k), th, el);
}

z3::expr Bv2IntTranslator::simplify_equality_mod(const z3::expr &eq) {
  Z3_decl_kind f = eq.arg(0).decl().decl_kind();

  // Early exit if the operator is not MOD
  if (f != Z3_OP_MOD) {
    return eq;
  }

  // Left-hand side of mod
  z3::expr lhs = eq.arg(0).arg(0);
  // Right-hand side of mod
  z3::expr rhs = eq.arg(0).arg(1);

  // Early exit if rhs is not a numeral and lhs is not a constant variable
  if (!is_const_variable(lhs) || !rhs.is_numeral()) {
    return eq;
  }

  assert(lhs.get_sort().is_int() && "Expected lhs to be of Int sort");
  Z3_ast key = (Z3_ast)lhs;
  auto it = m_int_var_bitwidth.find(key);
  assert(it != m_int_var_bitwidth.end() &&
         "Expected to find the bit-width of the variable");
  unsigned bv_size = it->second;
  uint64_t N = (uint64_t)1 << bv_size;

  // Early exit if rhs is not a numeral or not equal to N, or lhs is not a
  // constant variable
  if (rhs.get_numeral_uint64() != N) {
    return eq;
  }

  // The right-hand side of the equality (mod result)
  z3::expr m = eq.arg(1);

  // Early exit if m is not a numeral
  if (!m.is_numeral()) {
    return eq;
  }

  uint64_t value = m.get_numeral_uint64();
  // Early exit in case the value in the interval [0, N-1]
  if (value < 0 || value >= N) {
    return eq;
  }

  // Handle signed values
  if (m_is_signed) {
    uint64_t max_signed_val = ((uint64_t)1 << (bv_size - 1)) - 1;

    // If value is outside the signed range, adjust it
    if (value > max_signed_val) {
      value -= N;
    }

    // Return simplified equality
    return lhs == ctx.int_val((int64_t)value);
  }

  return lhs == ctx.int_val(value);
}
} // namespace lotus::mosaic
