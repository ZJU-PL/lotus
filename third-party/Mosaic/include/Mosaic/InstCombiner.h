#pragma once
#include <z3++.h>

namespace lotus::mosaic {
// A tiny "InstCombine"-style rewriter for Z3 Int expressions.
class InstCombiner {
private:
  z3::context &m_ctx;

  // Match t == (* -1 b) where all are integers and arity = 2
  // If match, set out_b = b and return true
  bool is_mone_times(const z3::expr &t, z3::expr &out_b) const;

  // Decompose integer "a - b" in common encodings:
  //   (- a b)                            -> a, b
  //   (+ a (* -1 b)) or (+ (* -1 b) a)   -> a, b
  bool decompose_sub(const z3::expr &t, z3::expr &a, z3::expr &b) const;

  // Visitors for specific operators
  z3::expr visitGE(const z3::func_decl &f, const z3::expr_vector &args);
  z3::expr visitLE(const z3::func_decl &f, const z3::expr_vector &args);

public:
  explicit InstCombiner(z3::context &ctx) : m_ctx(ctx) {}

  // Recursively rewrite 'e' and return the combined result.
  z3::expr combine(const z3::expr &e);
};
} // namespace lotus::mosaic