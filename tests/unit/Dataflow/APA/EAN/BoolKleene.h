#pragma once

// A concrete Kleene algebra used as a differential oracle for EAN tests:
// atoms map deterministically to boolean 3x3 matrices; join = OR, seq = boolean
// matmul, star = reflexive-transitive closure, zero = zero matrix, one =
// identity. Equality of the interpretation before/after an EAN transform
// witnesses semantic preservation. This algebra satisfies left/right
// distributivity, so it is a valid oracle for factorization.

#include <array>
#include <cstdint>

#include "Dataflow/APA/Core/PathExpr.h"

namespace lotus_test_ean {

constexpr int N = 3;
struct Mat {
  std::array<std::array<bool, N>, N> a{};
  bool operator==(const Mat &o) const { return a == o.a; }
  bool operator!=(const Mat &o) const { return !(*this == o); }
};

inline Mat zeroM() { return Mat{}; }
inline Mat oneM() {
  Mat m;
  for (int i = 0; i < N; ++i) m.a[i][i] = true;
  return m;
}
inline Mat orM(const Mat &x, const Mat &y) {
  Mat r;
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) r.a[i][j] = x.a[i][j] || y.a[i][j];
  return r;
}
inline Mat mulM(const Mat &x, const Mat &y) {
  Mat r;
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) {
      bool s = false;
      for (int k = 0; k < N; ++k) s = s || (x.a[i][k] && y.a[k][j]);
      r.a[i][j] = s;
    }
  return r;
}
inline Mat closureM(const Mat &x) {
  Mat r = oneM(), p = oneM();
  for (int it = 0; it < N + 1; ++it) {
    p = mulM(p, x);
    r = orM(r, p);
  }
  return r;
}
inline Mat gen(int id) {
  Mat m;
  std::uint32_t h = static_cast<std::uint32_t>(id) * 2654435761u + 0x9e3779b9u;
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) m.a[i][j] = ((h >> (i * N + j)) & 1u) != 0;
  return m;
}

// Interpret a PathExprFactory<int> expression (atoms are generator indices).
inline Mat evalRef(const typename elimination::PathExprFactory<int>::Ref &e) {
  using Kind = elimination::PathExprFactory<int>::Kind;
  switch (e->K) {
  case Kind::Zero: return zeroM();
  case Kind::One: return oneM();
  case Kind::Atom: return gen(*e->Transfer);
  case Kind::Union: return orM(evalRef(e->L), evalRef(e->R));
  case Kind::Concat: return mulM(evalRef(e->L), evalRef(e->R));
  case Kind::Star: return closureM(evalRef(e->L));
  }
  return zeroM();
}

} // namespace lotus_test_ean
