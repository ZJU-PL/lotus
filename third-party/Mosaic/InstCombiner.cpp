#include "Mosaic/InstCombiner.h"

#include <vector>

namespace lotus::mosaic {
// Returns true if e is an integer expression
static inline bool isInt(const z3::expr &e) { return e.get_sort().is_int(); }

bool InstCombiner::is_mone_times(const z3::expr &t, z3::expr &out_b) const {
  if (!t.is_app() || !isInt(t))
    return false;

  if (static_cast<Z3_decl_kind>(t.decl().decl_kind()) != Z3_OP_MUL)
    return false;

  if (t.num_args() != 2)
    return false;

  z3::expr m1 = m_ctx.int_val(-1);

  if (z3::eq(t.arg(0), m1)) {
    out_b = t.arg(1);
    return true;
  }

  if (z3::eq(t.arg(1), m1)) {
    out_b = t.arg(0);
    return true;
  }

  return false;
}

bool InstCombiner::decompose_sub(const z3::expr &t, z3::expr &a,
                                 z3::expr &b) const {
  if (!t.is_app() || !isInt(t))
    return false;

  const Z3_decl_kind k = static_cast<Z3_decl_kind>(t.decl().decl_kind());

  if (k == Z3_OP_SUB && t.num_args() == 2) {
    a = t.arg(0);
    b = t.arg(1);
    return true;
  }

  if (k == Z3_OP_ADD && t.num_args() == 2) {
    z3::expr nb(m_ctx);
    if (is_mone_times(t.arg(0), nb)) {
      a = t.arg(1);
      b = nb;
      return true;
    }
    if (is_mone_times(t.arg(1), nb)) {
      a = t.arg(0);
      b = nb;
      return true;
    }
  }

  return false;
}

//==============================================================================
//                              PUBLIC METHODS
//==============================================================================
z3::expr InstCombiner::combine(const z3::expr &e) {
  if (!e.is_app() || e.is_const())
    return e;

  // Post-order: combine children first
  z3::expr_vector new_args(m_ctx);
  const unsigned n = e.num_args();
  for (unsigned i = 0; i < n; ++i) {
    new_args.push_back(combine(e.arg(i)));
  }

  // Dispatch by operator kind (InstCombine-style)
  switch (static_cast<Z3_decl_kind>(e.decl().decl_kind())) {
  case Z3_OP_GE:
    return visitGE(e.decl(), new_args);
  case Z3_OP_LE:
    return visitLE(e.decl(), new_args);
  default: {
    return e.decl()(new_args);
  }
  }
}

// (>= (x - y) 1)  ==>  (> x y)
z3::expr InstCombiner::visitGE(const z3::func_decl &f,
                               const z3::expr_vector &args) {
  if (args.size() == 2 && isInt(args[0]) && isInt(args[1])) {
    if (z3::eq(args[1], m_ctx.int_val(1))) {
      z3::expr a(m_ctx), b(m_ctx);
      if (decompose_sub(args[0], a, b))
        return a > b;
    }
  }
  return f(args); // no change
}

// (<= (x - y) -1) ==>  (< x y)
z3::expr InstCombiner::visitLE(const z3::func_decl &f,
                               const z3::expr_vector &args) {
  if (args.size() == 2 && isInt(args[0]) && isInt(args[1])) {
    if (z3::eq(args[1], m_ctx.int_val(-1))) {
      z3::expr a(m_ctx), b(m_ctx);
      if (decompose_sub(args[0], a, b))
        return a < b;
    }
  }
  return f(args); // no change
}

} // namespace lotus::mosaic