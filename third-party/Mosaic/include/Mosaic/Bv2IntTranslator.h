//------------------------------------------------------------------------------
// Bv2IntTranslator.h
// A header file for the Bv2IntTranslator class, which translates
// bit-vector expressions to integer expressions in Z3.
// Based on the paper: Bit-Precise Reasnoning via Int-Blasting by Zohar et al.
//------------------------------------------------------------------------------

#pragma once
#include "Mosaic/MosaicUtils.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include <z3++.h>

namespace lotus::mosaic {

class Bv2IntTranslator {
  z3::context &ctx;
  bool m_is_signed;   // Whether to treat bit-vectors as signed or unsigned
  bool m_simplify;    // Whether to simplify the translated expressions
  bool m_no_overflow; // Whether to consider overflow cases

  // Map each original BV expr AST to its translated Int expr
  // This is used to cache results of translations
  std::unordered_map<Z3_ast, z3::expr, AstHash, AstEq> m_translate;
  // Map each new int variable to its original bit-width
  std::unordered_map<Z3_ast, unsigned, AstHash, AstEq> m_int_var_bitwidth;
  // collected BV variables (are collected to later be used to create lemmas)
  z3::expr_vector m_vars;
  // A map which tells us where to map each variable we find
  // in the expressions given through the translate method
  VarMap m_bv2int_var_map;

  // A set of lemmas specifying the bounds on variables
  VarLemmaMap m_lemmas;

  const std::string fresh_var_name = "__int_var__";
  unsigned var_count = 0;

  z3::expr bseli(const z3::expr &e, unsigned i);
  z3::expr umod(const z3::expr &e, unsigned k);
  z3::expr uts(const z3::expr &e, unsigned k);
  z3::expr stu(const z3::expr &e, unsigned k);

  // Custom implementation of common operations which could simplify
  // the final expression
  z3::expr pow2(const z3::expr &e);
  z3::expr if_eq(const z3::expr &e, uint64_t k, const z3::expr &th,
                 const z3::expr &el);

  // Helper utility functions
  bool is_basic(const z3::expr &e) const;
  bool is_casting(const z3::expr &e) const;
  bool is_special_basic(const z3::expr &e) const;
  bool is_bv_relation(const z3::expr &e) const;
  bool is_const_variable(const z3::expr &e) const;

  // Core translation routines
  z3::expr translate_bv(const z3::expr &e);
  z3::expr translate_cast(const z3::expr &e);
  z3::expr translate_bv_rel(const z3::expr &e);
  z3::expr translate_basic(const z3::expr &e);
  z3::expr translate_special_basic(const z3::expr &e);
  z3::expr translate_const_variable(const z3::expr &e);
  // General translation routine, where we no overflow is not guaranteed
  z3::expr translate_overflow(const z3::expr &e);

  // General translation routine, where no overflow is guaranteed
  z3::expr translate_no_overflow(const z3::expr &e);
  z3::expr translate_bv_no_overflow(const z3::expr &e);

  z3::expr create_bitwise_sum(const Z3_decl_kind &f, const z3::expr &arg1,
                              const z3::expr &arg2, unsigned k);
  void create_bound_lemma(z3::expr &var, unsigned k);

  /// @brief  The function's goal is to simplify the following equalities:
  /// Signed:
  ///         (x mod N) == N-3 --> x == -3
  /// Unsigned:
  ///         (x mod N) == N-3 --> x == N-3
  /// @param eq The original modulo equality
  /// @return The simplified expression if it's possible to simplify
  z3::expr simplify_equality_mod(const z3::expr &eq);

public:
  explicit Bv2IntTranslator(z3::context &c, bool is_signed,
                            bool simplify = true, bool no_overflow = false,
                            const VarMap &bv2int_var_map = VarMap());
  void reset();

  // Translate any expr; caches results in m_translate
  z3::expr translate(const z3::expr &e);

  // Accessors for the collected vars and lemmas
  z3::expr_vector vars() const { return m_vars; }
  const VarLemmaMap &lemmas() const { return m_lemmas; }
  const VarMap &bv2int_var_map() const { return m_bv2int_var_map; }
};
} // namespace lotus::mosaic
