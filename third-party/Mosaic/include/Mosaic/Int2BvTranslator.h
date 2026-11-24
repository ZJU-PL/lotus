//------------------------------------------------------------------------------
// Int2BvTranslator.h
// A header file for the Int2BvTranslator class, which translates
// integer expressions to bit-vector expressions in Z3.
//------------------------------------------------------------------------------

#pragma once
#include "Mosaic/Int2BvOverflowAnalyzer.h"
#include "Mosaic/Int2BvPreprocessor.h"
#include "Mosaic/Z3Utils.h"

#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <vector>

#include <z3++.h>

namespace lotus::mosaic {

class Int2BvTranslator {
  z3::context &ctx;
  unsigned m_bv_size;      // Size of the BV type to translate to
  bool m_is_signed;        // Whether to treat integers as signed or unsigned
  bool m_simplify;         // Whether to simplify the translated expressions
  bool m_force_preprocess; // Whether to always preprocess expressions when
                           // handling overflow
  // A map which tells us where to map each variable we find
  // in the expressions given through the translate method
  VarMap m_int2bv_var_map;

  // Size of extended BV width for vars and constants.
  // By default, it is equal to m_bv_size.
  // It's used to avoid expressions that are prone to overflow/underflow.
  unsigned m_extended_bv_size;
  // Whether to treat variables as signed or unsigned
  // when extending them to m_extended_bv_size.
  bool m_is_vars_signed;

  z3::expr_vector m_vars;
  const std::string fresh_var_name = "__bv_var__";
  unsigned var_count = 0;

  // This map is used to cache translations
  std::unordered_map<Z3_ast, z3::expr, AstHash, AstEq> m_translate;

  bool is_special_basic(const z3::expr &e) const;
  bool is_basic(const z3::expr &e) const;
  bool is_int_relation(const z3::expr &e) const;

  void determine_extension_config(const z3::expr &e);

  // Core translation routines
  z3::expr translate_int(const z3::expr &e);
  z3::expr translate_basic(const z3::expr &e);
  z3::expr translate_special_basic(const z3::expr &e);
  z3::expr translate_const_variable(const z3::expr &e);

  z3::expr translate_aux(const z3::expr &e);

public:
  explicit Int2BvTranslator(z3::context &c, bool is_signed, unsigned bv_size,
                            bool force_preprocess, bool simplify = true,
                            const VarMap &bv2int_var_map = VarMap());

  // This must be invoked before starting a new translation
  // It clears the cache and resets the translator state
  void reset();

  // Translate any expr; caches results in m_translate
  z3::expr translate(const z3::expr &e, bool handle_overflow = true);

  // Accessors for the collected vars
  const z3::expr_vector &vars() const { return m_vars; }
};
} // namespace lotus::mosaic
