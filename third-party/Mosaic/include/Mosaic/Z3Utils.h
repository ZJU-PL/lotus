#pragma once
#include <cassert>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include <z3++.h>

namespace lotus::mosaic::utils {

inline z3::expr_vector get_args(const z3::expr &expr) {
  z3::expr_vector args(expr.ctx());
  for (unsigned i = 0; i < expr.num_args(); ++i)
    args.push_back(expr.arg(i));
  return args;
}

template <typename S, typename T> bool any_of(S const &set, T const &p) {
  for (auto const &s : set)
    if (p(s))
      return true;
  return false;
}

inline int get_num_conjuncts(const z3::expr &e) {
  if (e.is_and()) {
    return e.num_args();
  }
  return 1;
}

/// @brief Gets all the conjuncts of an "and" expression as an expr_vector.
/// If the expression is not an "and", returns a vector with the expression
/// itself.
/// @param e The expression to get the conjuncts from.
/// @return An expr_vector containing the conjuncts.
z3::expr_vector get_conjuncts(const z3::expr &e);

inline int get_num_disjuncts(const z3::expr &e) {
  if (e.is_or()) {
    return e.num_args();
  }
  return 1;
}

/// @brief Checks if the given expression is an uninterpreted predicate.
/// @param e The expression to check.
/// @return True if the expression is an uninterpreted predicate, false
/// otherwise.
bool is_uninterpreted_predicate(const z3::expr &e);

/// @brief Sign-extends the given raw bit-vector value of the specified width.
/// @param raw The raw bit-vector value.
/// @param width The width of the bit-vector.
/// @return The sign-extended value.
int64_t sign_extend(uint64_t raw, unsigned width);

/// @brief Gets the lower bound of a signed bit-vector of the given size.
/// @param bv_size The size of the bit-vector.
/// @return The lower bound.
int64_t get_signed_bv_lower_bound(unsigned bv_size);

/// @brief Gets the upper bound of a signed bit-vector of the given size.
/// @param bv_size The size of the bit-vector.
/// @return The upper bound.
int64_t get_signed_bv_upper_bound(unsigned bv_size);

/// @brief Gets the upper bound of an unsigned bit-vector of the given size.
/// @param bv_size The size of the bit-vector.
/// @return The upper bound.
uint64_t get_unsigned_bv_upper_bound(unsigned bv_size);

/// @brief Gets the lower bound of an unsigned bit-vector of the given size.
/// @param bv_size The size of the bit-vector.
/// @return The lower bound.
uint64_t get_unsigned_bv_lower_bound(unsigned bv_size);

/// @brief Constructs a Z3 BV application based on the given declaration kind
/// and arguments.
/// @param ctx The Z3 context.
/// @param decl_kind The declaration kind.
/// @param args The arguments for the expression.
/// @return The constructed Z3 expression.
z3::expr get_bv_app_based_on_decl(z3::context &ctx,
                                  const Z3_decl_kind &decl_kind,
                                  const z3::expr_vector &args);
} // namespace lotus::mosaic::utils
