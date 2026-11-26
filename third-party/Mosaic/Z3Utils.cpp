#include "Mosaic/Z3Utils.h"

namespace lotus::mosaic::utils {

/// @brief An auxiliary function for recursively getting conjuncts.
/// @param e The expression to analyze.
/// @param conjuncts The vector to store the resulting conjuncts.
static void get_conjuncts_impl(const z3::expr &e, z3::expr_vector &conjuncts) {
  if (e.is_and()) {
    for (unsigned i = 0; i < e.num_args(); ++i) {
      get_conjuncts_impl(e.arg(i), conjuncts);
    }
  } else {
    conjuncts.push_back(e);
  }
}

//==============================================================================
//                              PUBLIC METHODS
//==============================================================================
z3::expr_vector get_conjuncts(const z3::expr &e) {
  z3::expr_vector conjuncts(e.ctx());
  get_conjuncts_impl(e, conjuncts);
  return conjuncts;
}

int64_t sign_extend(uint64_t raw, unsigned width) {
  assert(width > 0 && width <= 63);
  // keep only the low 'width' bits
  uint64_t mask = (1ULL << width) - 1;
  int64_t u = raw & mask;
  // if top bit set, subtract 2^width
  if (u & (1ULL << (width - 1)))
    u -= (1ULL << width);
  return u;
}

bool is_uninterpreted_predicate(const z3::expr &e) {
  Z3_decl_kind kind = e.decl().decl_kind();
  if (kind == Z3_OP_UNINTERPRETED)
    return true;
#ifdef LOTUS_Z3_HAS_OP_RECURSIVE
  return kind == Z3_OP_RECURSIVE;
#else
  return false;
#endif
}

int64_t get_signed_bv_lower_bound(unsigned bv_size) {
  return -(int64_t(1) << (bv_size - 1));
}

int64_t get_signed_bv_upper_bound(unsigned bv_size) {
  return (int64_t(1) << (bv_size - 1)) - 1;
}

uint64_t get_unsigned_bv_upper_bound(unsigned bv_size) {
  if (bv_size >= 64)
    return std::numeric_limits<uint64_t>::max();
  return (uint64_t(1) << bv_size) - 1;
}

uint64_t get_unsigned_bv_lower_bound(unsigned bv_size) { return 0; }

z3::expr get_bv_app_based_on_decl(z3::context &ctx,
                                  const Z3_decl_kind &decl_kind,
                                  const z3::expr_vector &args) {
  switch (decl_kind) {
  // ------------------------------------------------------------------
  // Basic
  // ------------------------------------------------------------------
  case Z3_OP_EQ:
    return args[0] == args[1];
  case Z3_OP_DISTINCT:
    return z3::distinct(args);
  case Z3_OP_ITE:
    return z3::ite(args[0], args[1], args[2]);
  case Z3_OP_AND:
    return z3::mk_and(args);
  case Z3_OP_OR:
    return z3::mk_or(args);
  case Z3_OP_XOR:
    // Older Z3 C++ APIs do not provide a variadic mk_xor helper.
    if (args.empty())
      return ctx.bool_val(false);
    {
      z3::expr result = args[0];
      for (unsigned i = 1; i < args.size(); ++i)
        result = result != args[i];
      return result;
    }
  case Z3_OP_NOT:
    return !args[0];
  case Z3_OP_IMPLIES:
    return z3::implies(args[0], args[1]);

  // ------------------------------------------------------------------
  // Arithmetic
  // ------------------------------------------------------------------
  case Z3_OP_BNEG:
    return -args[0];
  case Z3_OP_BADD:
    return args[0] + args[1];
  case Z3_OP_BSUB:
    return args[0] - args[1];
  case Z3_OP_BMUL:
    return args[0] * args[1];

  // ------------------------------------------------------------------
  // Division and Remainder
  // ------------------------------------------------------------------
  case Z3_OP_BSDIV:
  case Z3_OP_BSDIV_I:
    return args[0] / args[1]; // operator/ is signed division in Z3 C++

  case Z3_OP_BUDIV:
  case Z3_OP_BUDIV_I:
    return z3::udiv(args[0], args[1]);

  case Z3_OP_BSREM:
  case Z3_OP_BSREM_I:
    return z3::srem(args[0], args[1]);

  case Z3_OP_BUREM:
  case Z3_OP_BUREM_I:
    return z3::urem(args[0], args[1]);

  case Z3_OP_BSMOD:
  case Z3_OP_BSMOD_I:
    return z3::smod(args[0], args[1]);

  // ------------------------------------------------------------------
  // Bitwise Logic
  // ------------------------------------------------------------------
  case Z3_OP_BNOT:
    return ~args[0];
  case Z3_OP_BAND:
    return args[0] & args[1];
  case Z3_OP_BOR:
    return args[0] | args[1];
  case Z3_OP_BXOR:
    return args[0] ^ args[1];
  case Z3_OP_BNAND:
    return ~(args[0] & args[1]);
  case Z3_OP_BNOR:
    return ~(args[0] | args[1]);
  case Z3_OP_BXNOR:
    return ~(args[0] ^ args[1]);

  // ------------------------------------------------------------------
  // Structural
  // ------------------------------------------------------------------
  case Z3_OP_CONCAT:
    return z3::concat(args[0], args[1]);

  // ------------------------------------------------------------------
  // Shifts
  // ------------------------------------------------------------------
  case Z3_OP_BSHL:
    return z3::shl(args[0], args[1]);
  case Z3_OP_BLSHR:
    return z3::lshr(args[0], args[1]);
  case Z3_OP_BASHR:
    return z3::ashr(args[0], args[1]);

  // ------------------------------------------------------------------
  // Comparisons
  // ------------------------------------------------------------------
  case Z3_OP_ULEQ:
    return z3::ule(args[0], args[1]);
  case Z3_OP_SLEQ:
    return args[0] <= args[1];
  case Z3_OP_UGEQ:
    return z3::uge(args[0], args[1]);
  case Z3_OP_SGEQ:
    return args[0] >= args[1];
  case Z3_OP_ULT:
    return z3::ult(args[0], args[1]);
  case Z3_OP_SLT:
    return args[0] < args[1];
  case Z3_OP_UGT:
    return z3::ugt(args[0], args[1]);
  case Z3_OP_SGT:
    return args[0] > args[1];
  default:
    throw z3::exception(
        "Unsupported declaration kind in get_bv_app_based_on_decl");
  }
}
} // namespace lotus::mosaic::utils
