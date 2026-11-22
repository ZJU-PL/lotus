#pragma once
// C++17 implementation of the Dyck-2 certificate systems in
// Chistikov, Majumdar, Schepper, "Subcubic Certificates for CFL Reachability".
// Vertices are [0,n). Labels: '(', ')', '[', ']'.

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <queue>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>

namespace cflcert {

enum class Label : uint8_t { Open1 = 0, Open2 = 1, Close1 = 2, Close2 = 3 };
inline bool isOpen(Label x) { return x == Label::Open1 || x == Label::Open2; }
inline bool matches(Label a, Label b) {
  return (a == Label::Open1 && b == Label::Close1) ||
         (a == Label::Open2 && b == Label::Close2);
}
struct Edge {
  int from, to;
  Label label;
};

struct Instance {
  int n = 0, source = 0, target = 0;
  std::vector<Edge> edges;
  void validate() const {
    // Pair identifiers are stored as int (u*n+v) throughout this
    // compact reference implementation. Reject sizes that could overflow.
    if (n <= 0 || n > 46340 || source < 0 || source >= n || target < 0 ||
        target >= n)
      throw std::invalid_argument(
          "invalid graph size/source/target (require 1 <= n <= 46340)");
    for (const auto &e : edges) {
      if (e.from < 0 || e.from >= n || e.to < 0 || e.to >= n)
        throw std::invalid_argument("edge endpoint out of range");
      if (static_cast<unsigned>(e.label) > static_cast<unsigned>(Label::Close2))
        throw std::invalid_argument("invalid edge label");
    }
  }
};

// ---------- Positive certificates: walk schemes ----------
// A nonterminal is the ordered pair (u,v). Each pair has exactly one rule.
// Split: (u,v)->(u,w)(w,v)
// Wrap:  (u,v)-> e (x,y) f, where e:u->x and f:y->v match
// Empty: (u,u)->epsilon
struct Rule {
  enum Kind : uint8_t { Split, Wrap, Empty } kind = Empty;
  int w = -1;                        // Split intermediate vertex
  int firstEdge = -1, lastEdge = -1; // Wrap edge indices
};
struct PositiveCertificate {
  // rule[u*n+v] is -1 if absent, otherwise index into rules.
  std::vector<Rule> rules;
  std::vector<int> ruleOfPair;
};

inline bool verifyPositive(const Instance &g, const PositiveCertificate &c) {
  g.validate();
  const int n = g.n, N = n * n;
  if ((int)c.ruleOfPair.size() != N)
    return false;
  if (c.rules.size() > (size_t)N)
    return false;
  std::vector<int> indeg(N, 0);
  std::vector<std::vector<int>> adj(N);
  std::vector<char> ruleUsed(c.rules.size(), 0);
  int root = g.source * n + g.target;
  if (c.ruleOfPair[root] < 0 || c.ruleOfPair[root] >= (int)c.rules.size())
    return false;
  for (int id = 0; id < N; ++id) {
    int ri = c.ruleOfPair[id];
    if (ri == -1)
      continue;
    if (ri < 0 || ri >= (int)c.rules.size())
      return false;
    if (ruleUsed[ri])
      return false; // one distinct production per nonterminal
    ruleUsed[ri] = 1;
    int u = id / n, v = id % n;
    const Rule &r = c.rules[ri];
    auto addDep = [&](int a, int b) {
      if (a < 0 || a >= n || b < 0 || b >= n)
        return false;
      int d = a * n + b;
      if (c.ruleOfPair[d] < 0)
        return false;
      adj[id].push_back(d);
      ++indeg[d];
      return true;
    };
    if (r.kind == Rule::Empty) {
      if (u != v)
        return false;
    } else if (r.kind == Rule::Split) {
      if (r.w < 0 || r.w >= n)
        return false;
      if (!addDep(u, r.w) || !addDep(r.w, v))
        return false;
    } else if (r.kind == Rule::Wrap) {
      if (r.firstEdge < 0 || r.firstEdge >= (int)g.edges.size() ||
          r.lastEdge < 0 || r.lastEdge >= (int)g.edges.size())
        return false;
      const Edge &a = g.edges[r.firstEdge];
      const Edge &b = g.edges[r.lastEdge];
      if (a.from != u || b.to != v || !matches(a.label, b.label))
        return false;
      if (!addDep(a.to, b.from))
        return false;
    } else
      return false;
  }
  // Every encoded production must belong to exactly one nonterminal.
  for (char usedRule : ruleUsed)
    if (!usedRule)
      return false;
  // Definition 3 does not require every nonterminal to be reachable
  // from the axiom. Do not impose a stronger condition here: the grammar
  // may contain harmless unreachable nonterminals, provided their rules
  // are well-formed and the complete dependency graph is acyclic.

  // Acyclic dependency graph; reverse topological induction proves validity.
  std::queue<int> q;
  int used = 0;
  for (int i = 0; i < N; ++i)
    if (c.ruleOfPair[i] != -1) {
      ++used;
      if (indeg[i] == 0)
        q.push(i);
    }
  int popped = 0;
  while (!q.empty()) {
    int x = q.front();
    q.pop();
    ++popped;
    for (int y : adj[x])
      if (--indeg[y] == 0)
        q.push(y);
  }
  return popped == used;
}

// Builds a certificate if reachable. This is a straightforward cubic-style
// saturation constructor (not the subcubic verifier). Each newly discovered
// pair gets a rule whose dependencies were discovered earlier.
inline bool buildPositive(const Instance &g, PositiveCertificate &out) {
  g.validate();
  const int n = g.n, N = n * n;
  out.ruleOfPair.assign(N, -1);
  out.rules.clear();
  std::vector<char> known(N, 0);
  std::vector<std::vector<int>> outByLabel(4), inByLabel(4);
  for (int i = 0; i < (int)g.edges.size(); ++i) {
    const auto &e = g.edges[i];
    outByLabel[(int)e.label].push_back(i);
    inByLabel[(int)e.label].push_back(i);
  }
  auto add = [&](int u, int v, Rule r) -> bool {
    int id = u * n + v;
    if (known[id])
      return false;
    known[id] = 1;
    out.ruleOfPair[id] = (int)out.rules.size();
    out.rules.push_back(r);
    return true;
  };
  for (int i = 0; i < n; ++i)
    add(i, i, Rule{Rule::Empty, -1, -1, -1});
  bool changed = true;
  while (changed) {
    changed = false;
    // Concatenation: known (u,w) and (w,v) imply (u,v).
    for (int u = 0; u < n; ++u)
      for (int w = 0; w < n; ++w)
        if (known[u * n + w])
          for (int v = 0; v < n; ++v)
            if (known[w * n + v] && !known[u * n + v]) {
              changed |= add(u, v, Rule{Rule::Split, w, -1, -1});
            }
    // Matched wrapping edges around an already known inner pair.
    for (int ei = 0; ei < (int)g.edges.size(); ++ei) {
      const auto &e = g.edges[ei];
      if (!isOpen(e.label))
        continue;
      for (int fi = 0; fi < (int)g.edges.size(); ++fi) {
        const auto &f = g.edges[fi];
        if (!matches(e.label, f.label))
          continue;
        if (known[e.to * n + f.from] && !known[e.from * n + f.to])
          changed |= add(e.from, f.to, Rule{Rule::Wrap, -1, ei, fi});
      }
    }
  }
  if (!known[g.source * n + g.target])
    return false;
  // Drop nonterminals not reachable from the axiom; the paper's walk
  // schemes contain only useful nonterminals.
  std::vector<char> reach(N, 0);
  std::vector<int> stack{g.source * n + g.target};
  reach[stack.back()] = 1;
  while (!stack.empty()) {
    int id = stack.back();
    stack.pop_back();
    const Rule &r = out.rules[out.ruleOfPair[id]];
    auto visit = [&](int a, int b) {
      int d = a * n + b;
      if (!reach[d])
        reach[d] = 1, stack.push_back(d);
    };
    if (r.kind == Rule::Split) {
      visit(id / n, r.w);
      visit(r.w, id % n);
    } else if (r.kind == Rule::Wrap) {
      visit(g.edges[r.firstEdge].to, g.edges[r.lastEdge].from);
    }
  }
  for (int id = 0; id < N; ++id)
    if (!reach[id])
      out.ruleOfPair[id] = -1;
  // Compact the production array so every encoded production belongs to
  // exactly one retained nonterminal (no unreachable/dead certificate data).
  std::vector<Rule> compact;
  std::vector<int> remap(out.rules.size(), -1);
  for (int id = 0; id < N; ++id)
    if (out.ruleOfPair[id] >= 0) {
      int old = out.ruleOfPair[id];
      if (remap[old] < 0) {
        remap[old] = (int)compact.size();
        compact.push_back(out.rules[old]);
      }
      out.ruleOfPair[id] = remap[old];
    }
  out.rules.swap(compact);
  return verifyPositive(g, out);
}

// ---------- Negative certificates: separators ----------
// Six n*n matrices: S, SS, (S, [S, (S), [S].
// Entries are nonnegative integers; S is Boolean.
using Matrix = std::vector<uint64_t>;
struct Separator {
  Matrix S, SS, Open1S, Open2S, Open1SClose1, Open2SClose2;
};
inline Matrix zeroMatrix(int n) { return Matrix((size_t)n * n, 0); }
inline uint64_t &at(Matrix &a, int n, int i, int j) {
  return a[(size_t)i * n + j];
}
inline uint64_t at(const Matrix &a, int n, int i, int j) {
  return a[(size_t)i * n + j];
}

// Exact Strassen backend. Boost cpp_int prevents overflow in Strassen's
// signed intermediate sums; the public Matrix remains uint64_t. Results that
// do not fit uint64_t are rejected instead of silently wrapping.
using BigInt = boost::multiprecision::cpp_int;
using BigMatrix = std::vector<BigInt>;
inline BigMatrix bigAdd(const BigMatrix &a, const BigMatrix &b, int n,
                        int sign = 1) {
  BigMatrix c((size_t)n * n);
  for (size_t i = 0; i < c.size(); ++i)
    c[i] = a[i] + sign * b[i];
  return c;
}
inline BigMatrix bigStrassen(const BigMatrix &A, const BigMatrix &B, int n,
                             int cutoff = 32) {
  if (n <= 0 || (n & (n - 1)) != 0 || cutoff < 1 || A.size() != (size_t)n * n ||
      B.size() != (size_t)n * n)
    throw std::invalid_argument("Strassen requires equally sized power-of-two "
                                "matrices and positive cutoff");
  BigMatrix C((size_t)n * n);
  if (n <= cutoff) {
    for (int i = 0; i < n; ++i)
      for (int k = 0; k < n; ++k) {
        const BigInt &x = A[(size_t)i * n + k];
        if (x == 0)
          continue;
        for (int j = 0; j < n; ++j)
          C[(size_t)i * n + j] += x * B[(size_t)k * n + j];
      }
    return C;
  }
  int h = n / 2;
  auto quad = [&](const BigMatrix &X, int r, int c) {
    BigMatrix q((size_t)h * h);
    for (int i = 0; i < h; ++i)
      for (int j = 0; j < h; ++j)
        q[(size_t)i * h + j] = X[(size_t)(r + i) * n + c + j];
    return q;
  };
  BigMatrix A11 = quad(A, 0, 0), A12 = quad(A, 0, h), A21 = quad(A, h, 0),
            A22 = quad(A, h, h);
  BigMatrix B11 = quad(B, 0, 0), B12 = quad(B, 0, h), B21 = quad(B, h, 0),
            B22 = quad(B, h, h);
  BigMatrix M1 =
      bigStrassen(bigAdd(A11, A22, h), bigAdd(B11, B22, h), h, cutoff);
  BigMatrix M2 = bigStrassen(bigAdd(A21, A22, h), B11, h, cutoff);
  BigMatrix M3 = bigStrassen(A11, bigAdd(B12, B22, h, -1), h, cutoff);
  BigMatrix M4 = bigStrassen(A22, bigAdd(B21, B11, h, -1), h, cutoff);
  BigMatrix M5 = bigStrassen(bigAdd(A11, A12, h), B22, h, cutoff);
  BigMatrix M6 =
      bigStrassen(bigAdd(A21, A11, h, -1), bigAdd(B11, B12, h), h, cutoff);
  BigMatrix M7 =
      bigStrassen(bigAdd(A12, A22, h, -1), bigAdd(B21, B22, h), h, cutoff);
  BigMatrix C11 = bigAdd(bigAdd(bigAdd(M1, M4, h), M5, h, -1), M7, h);
  BigMatrix C12 = bigAdd(M3, M5, h);
  BigMatrix C21 = bigAdd(M2, M4, h);
  BigMatrix C22 = bigAdd(bigAdd(bigAdd(M1, M2, h, -1), M3, h), M6, h);
  auto put = [&](const BigMatrix &Q, int r, int c) {
    for (int i = 0; i < h; ++i)
      for (int j = 0; j < h; ++j)
        C[(size_t)(r + i) * n + c + j] = Q[(size_t)i * h + j];
  };
  put(C11, 0, 0);
  put(C12, 0, h);
  put(C21, h, 0);
  put(C22, h, h);
  return C;
}
inline Matrix multiplyClassicalExact(const Matrix &a, const Matrix &b, int n) {
  if (n < 0 || a.size() != (size_t)n * n || b.size() != (size_t)n * n)
    throw std::invalid_argument("matrix dimension mismatch");
  Matrix out((size_t)n * n, 0);
  uint64_t maxA = 0, maxB = 0;
  for (uint64_t x : a)
    maxA = std::max(maxA, x);
  for (uint64_t x : b)
    maxB = std::max(maxB, x);
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  const bool safe = (maxA == 0 || maxB == 0 ||
                     maxA <= limit / maxB / (uint64_t)std::max(1, n));
  if (safe) {
    for (int i = 0; i < n; ++i)
      for (int k = 0; k < n; ++k) {
        uint64_t x = a[(size_t)i * n + k];
        if (!x)
          continue;
        for (int j = 0; j < n; ++j)
          out[(size_t)i * n + j] += x * b[(size_t)k * n + j];
      }
    return out;
  }
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) {
      BigInt sum = 0;
      for (int k = 0; k < n; ++k)
        sum += BigInt(a[(size_t)i * n + k]) * b[(size_t)k * n + j];
      if (sum > limit)
        throw std::overflow_error("exact matrix product does not fit uint64_t");
      out[(size_t)i * n + j] = sum.convert_to<uint64_t>();
    }
  return out;
}
inline Matrix multiplyFastExact(const Matrix &a, const Matrix &b, int n,
                                int cutoff = 32) {
  if (cutoff < 1)
    throw std::invalid_argument("Strassen cutoff must be positive");
  if (n < 0 || a.size() != (size_t)n * n || b.size() != (size_t)n * n)
    throw std::invalid_argument("matrix dimension mismatch");
  if (n == 0)
    return {};
  if (n <= cutoff)
    return multiplyClassicalExact(a, b, n);
  int m = 1;
  while (m < n) {
    if (m > std::numeric_limits<int>::max() / 2)
      throw std::length_error("matrix too large");
    m *= 2;
  }
  BigMatrix A((size_t)m * m), B((size_t)m * m);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) {
      A[(size_t)i * m + j] = at(a, n, i, j);
      B[(size_t)i * m + j] = at(b, n, i, j);
    }
  BigMatrix C = bigStrassen(A, B, m, cutoff);
  Matrix out((size_t)n * n);
  const BigInt limit = std::numeric_limits<uint64_t>::max();
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) {
      const BigInt &x = C[(size_t)i * m + j];
      if (x < 0 || x > limit)
        throw std::overflow_error("exact matrix product does not fit uint64_t");
      out[(size_t)i * n + j] = x.convert_to<uint64_t>();
    }
  return out;
}
inline Matrix multiply(const Matrix &a, const Matrix &b, int n) {
  return multiplyFastExact(a, b, n);
}
inline Matrix booleanize(const Matrix &a) {
  Matrix b = a;
  for (auto &x : b)
    x = (x != 0);
  return b;
}
inline bool boolLeq(const Matrix &a, const Matrix &b) {
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i] && !b[i])
      return false;
  return true;
}
inline Matrix adjacency(const Instance &g, Label l) {
  Matrix a = zeroMatrix(g.n);
  for (const auto &e : g.edges)
    if (e.label == l)
      at(a, g.n, e.from, e.to) = 1;
  return a;
}

// Exact Dyck-2 reachability closure, used to construct a negative certificate.
// O(n^3) or worse in this simple implementation; certificate verification is
// the subcubic part of the paper.
inline void buildSeparator(const Instance &g, Separator &c) {
  g.validate();
  int n = g.n;
  Matrix A0 = adjacency(g, Label::Open1), A1 = adjacency(g, Label::Open2);
  Matrix B0 = adjacency(g, Label::Close1), B1 = adjacency(g, Label::Close2);
  c.S = zeroMatrix(n);
  for (int i = 0; i < n; ++i)
    at(c.S, n, i, i) = 1;
  bool changed = true;
  while (changed) {
    changed = false;
    Matrix ss = multiply(c.S, c.S, n);
    Matrix x0 = multiply(multiply(A0, c.S, n), B0, n);
    Matrix x1 = multiply(multiply(A1, c.S, n), B1, n);
    for (size_t z = 0; z < c.S.size(); ++z)
      if ((ss[z] || x0[z] || x1[z]) && !c.S[z])
        c.S[z] = 1, changed = true;
  }
  c.SS = multiply(c.S, c.S, n);
  c.Open1S = multiply(A0, c.S, n);
  c.Open2S = multiply(A1, c.S, n);
  c.Open1SClose1 = multiply(c.Open1S, B0, n);
  c.Open2SClose2 = multiply(c.Open2S, B1, n);
}

// Deterministic direct checker. Uses five exact matrix products O(n^3).
inline bool verifyNegativeDeterministic(const Instance &g, const Separator &c) {
  g.validate();
  int n = g.n;
  size_t z = (size_t)n * n;
  if (c.S.size() != z || c.SS.size() != z || c.Open1S.size() != z ||
      c.Open2S.size() != z || c.Open1SClose1.size() != z ||
      c.Open2SClose2.size() != z)
    return false;
  for (auto x : c.S)
    if (x > 1)
      return false;
  const uint64_t cap = (uint64_t)n * (uint64_t)n;
  for (const Matrix *m :
       {&c.SS, &c.Open1S, &c.Open2S, &c.Open1SClose1, &c.Open2SClose2})
    for (uint64_t x : *m)
      if (x > cap)
        return false;
  Matrix I = zeroMatrix(n);
  for (int i = 0; i < n; ++i)
    at(I, n, i, i) = 1;
  if (!boolLeq(I, c.S))
    return false;
  Matrix A0 = adjacency(g, Label::Open1), A1 = adjacency(g, Label::Open2);
  Matrix B0 = adjacency(g, Label::Close1), B1 = adjacency(g, Label::Close2);
  if (multiply(c.S, c.S, n) != c.SS)
    return false;
  if (multiply(A0, c.S, n) != c.Open1S)
    return false;
  if (multiply(A1, c.S, n) != c.Open2S)
    return false;
  if (multiply(c.Open1S, B0, n) != c.Open1SClose1)
    return false;
  if (multiply(c.Open2S, B1, n) != c.Open2SClose2)
    return false;
  if (!boolLeq(c.SS, c.S) || !boolLeq(c.Open1SClose1, c.S) ||
      !boolLeq(c.Open2SClose2, c.S))
    return false;
  return at(c.S, n, g.source, g.target) == 0;
}

// Freivalds check for X*Y=Z. One-sided error: a true equality always passes.
// Repetition count r gives error <= 2^-r for an individual product.
inline bool freivalds(const Matrix &X, const Matrix &Y, const Matrix &Z, int n,
                      int repetitions, std::mt19937_64 &rng) {
  if (n <= 0 || repetitions <= 0 || X.size() != (size_t)n * n ||
      Y.size() != (size_t)n * n || Z.size() != (size_t)n * n)
    return false;
  std::uniform_int_distribution<int> bit(0, 1);
  // The paper bounds every certificate entry by n^2. With the explicit
  // n <= 46340 input guard, the largest Freivalds accumulator is bounded
  // by n^4 < 2^64, so uint64_t arithmetic is exact (no modular wraparound).
  std::vector<uint64_t> v(n), yv(n), xv(n), zv(n);
  for (int rep = 0; rep < repetitions; ++rep) {
    for (auto &x : v)
      x = static_cast<uint64_t>(bit(rng));
    std::fill(yv.begin(), yv.end(), 0);
    std::fill(xv.begin(), xv.end(), 0);
    std::fill(zv.begin(), zv.end(), 0);
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < n; ++j) {
        yv[i] += at(Y, n, i, j) * v[j];
        zv[i] += at(Z, n, i, j) * v[j];
      }
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < n; ++j)
        xv[i] += at(X, n, i, j) * yv[j];
    if (xv != zv)
      return false;
  }
  return true;
}

// Randomized checker in O(n^2) per repetition/product. It follows Theorem 10:
// four repetitions per product => union-bound error <= 5/16.
// With separator bounds, all tested products and Freivalds vector products
// fit uint64_t for every realistically allocatable dense n-by-n instance.
inline bool
verifyNegativeRandomized(const Instance &g, const Separator &c,
                         uint64_t seed = std::numeric_limits<uint64_t>::max(),
                         int repetitions = 4) {
  g.validate();
  int n = g.n;
  size_t z = (size_t)n * n;
  if (c.S.size() != z || c.SS.size() != z || c.Open1S.size() != z ||
      c.Open2S.size() != z || c.Open1SClose1.size() != z ||
      c.Open2SClose2.size() != z)
    return false;
  for (auto x : c.S)
    if (x > 1)
      return false;
  const uint64_t cap = (uint64_t)n * (uint64_t)n;
  for (const Matrix *m :
       {&c.SS, &c.Open1S, &c.Open2S, &c.Open1SClose1, &c.Open2SClose2})
    for (uint64_t x : *m)
      if (x > cap)
        return false;
  Matrix I = zeroMatrix(n);
  for (int i = 0; i < n; ++i)
    at(I, n, i, i) = 1;
  if (!boolLeq(I, c.S) || !boolLeq(c.SS, c.S) ||
      !boolLeq(c.Open1SClose1, c.S) || !boolLeq(c.Open2SClose2, c.S) ||
      at(c.S, n, g.source, g.target) != 0)
    return false;
  Matrix A0 = adjacency(g, Label::Open1), A1 = adjacency(g, Label::Open2);
  Matrix B0 = adjacency(g, Label::Close1), B1 = adjacency(g, Label::Close2);
  // Use fresh random coins by default. Supplying an explicit seed is useful
  // for reproducible tests, but should not be used against an adversarial
  // certificate producer who knows the seed in advance.
  std::mt19937_64 rng;
  if (seed == std::numeric_limits<uint64_t>::max()) {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
    rng.seed(seq);
  } else {
    rng.seed(seed);
  }
  return freivalds(c.S, c.S, c.SS, n, repetitions, rng) &&
         freivalds(A0, c.S, c.Open1S, n, repetitions, rng) &&
         freivalds(A1, c.S, c.Open2S, n, repetitions, rng) &&
         freivalds(c.Open1S, B0, c.Open1SClose1, n, repetitions, rng) &&
         freivalds(c.Open2S, B1, c.Open2SClose2, n, repetitions, rng);
}

// Optional exact decision routine for testing / certificate construction.
// Returns whether (source,target) is Dyck-2 reachable.
inline bool decideCubic(const Instance &g) {
  Separator c;
  buildSeparator(g, c);
  return at(c.S, g.n, g.source, g.target) != 0;
}

} // namespace cflcert
