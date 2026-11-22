#pragma once
// Section 5 of Chistikov-Majumdar-Schepper, arXiv:2102.13095.
// Pushdown non-reachability certificates (Proposition 19).
// C++17, exact nonnegative integer arithmetic.
#include "CFL/Certificates/DyckCertificates.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace cflcert::pda {

struct Transition {
  int fromState;             // p in Q
  int popSymbol;             // A in Gamma
  int toState;               // q in Q
  std::vector<int> pushWord; // epsilon, B, or BC; replacement word top-first
                             // order (leftmost is new top)
};
struct PAutomaton {
  int states = 0;          // S, numbered [0, states)
  std::vector<int> finals; // F subset S
  std::vector<std::vector<std::pair<int, int>>> edges; // from -> (symbol,to)
};
struct PDAInstance {
  int controlStates = 0; // Q = [0, controlStates)
  int stackAlphabet = 0; // Gamma = [0, stackAlphabet)
  std::vector<Transition> transitions;
  int initialState = 0;
  int initialStackSymbol = 0;
  PAutomaton target;
  bool targetIsNormalized = false; // set only by normalizePAutomaton()

  void validate() const {
    if (controlStates <= 0 || stackAlphabet <= 0 ||
        target.states < controlStates)
      throw std::invalid_argument("invalid PDA/P-automaton dimensions");
    const size_t g = static_cast<size_t>(stackAlphabet),
                 ss = static_cast<size_t>(target.states);
    const size_t limit = std::numeric_limits<size_t>::max();
    if (g > limit / g || g * g > limit / g || ss > limit / ss ||
        g > limit / (ss * ss) ||
        controlStates > std::numeric_limits<int>::max() - target.states)
      throw std::length_error(
          "PDA certificate dimensions overflow address space");
    const size_t combined = static_cast<size_t>(controlStates + target.states);
    if (combined > limit / combined || g > limit / (combined * combined) ||
        g * combined * combined > (size_t)std::numeric_limits<int>::max())
      throw std::length_error(
          "triplet CFG dimensions exceed supported index range");
    const size_t s2 = ss * ss, g3 = g * g * g;
    if (g3 > limit / s2)
      throw std::length_error(
          "PDA negative-certificate matrix family exceeds address space");
    if (initialState < 0 || initialState >= controlStates ||
        initialStackSymbol < 0 || initialStackSymbol >= stackAlphabet)
      throw std::invalid_argument("invalid initial configuration");
    if ((int)target.edges.size() != target.states)
      throw std::invalid_argument("P-automaton adjacency size mismatch");
    for (int f : target.finals)
      if (f < 0 || f >= target.states)
        throw std::invalid_argument("invalid final state");
    for (int p = 0; p < target.states; ++p)
      for (auto [a, q] : target.edges[p]) {
        if (a < 0 || a >= stackAlphabet || q < 0 || q >= target.states)
          throw std::invalid_argument("invalid P-automaton edge");
        if (targetIsNormalized && q < controlStates)
          throw std::invalid_argument(
              "normalized P-automaton has an edge into a control state");
      }
    for (const auto &t : transitions) {
      if (t.fromState < 0 || t.fromState >= controlStates || t.toState < 0 ||
          t.toState >= controlStates || t.popSymbol < 0 ||
          t.popSymbol >= stackAlphabet || t.pushWord.size() > 2)
        throw std::invalid_argument("invalid PDA transition");
      for (int a : t.pushWord)
        if (a < 0 || a >= stackAlphabet)
          throw std::invalid_argument("invalid pushed symbol");
    }
  }
};

// Put the target P-automaton into normal form: Q remains its set of initial
// states, every automaton transition goes to a fresh copy of its destination,
// and all subsequent transitions stay in that copied component. This avoids
// confusing a mid-word automaton state that happens to share an identifier
// with a PDA control state. The construction preserves the regular language
// accepted from every q in Q and increases |S| by at most a factor of two.
inline PDAInstance normalizePAutomaton(const PDAInstance &input) {
  input.validate();
  if (input.targetIsNormalized)
    return input;
  PDAInstance out = input;
  const int Q = input.controlStates, S = input.target.states;
  if (Q > std::numeric_limits<int>::max() - S)
    throw std::length_error("normalized P-automaton too large");
  out.target.states = Q + S;
  out.target.edges.assign(Q + S, {});
  out.target.finals.clear();
  out.targetIsNormalized = true;
  for (int u = 0; u < S; ++u) {
    for (auto [A, v] : input.target.edges[u]) {
      out.target.edges[Q + u].push_back({A, Q + v});
      if (u < Q)
        out.target.edges[u].push_back({A, Q + v});
    }
    if (std::find(input.target.finals.begin(), input.target.finals.end(), u) !=
        input.target.finals.end()) {
      out.target.finals.push_back(Q + u);
      if (u < Q)
        out.target.finals.push_back(u); // preserve acceptance of epsilon
    }
  }
  std::sort(out.target.finals.begin(), out.target.finals.end());
  out.target.finals.erase(
      std::unique(out.target.finals.begin(), out.target.finals.end()),
      out.target.finals.end());
  out.validate();
  return out;
}

using Mat = cflcert::Matrix;
inline Mat zeros(int s) { return Mat((size_t)s * s, 0); }
inline uint64_t &cell(Mat &a, int s, int i, int j) {
  return a[(size_t)i * s + j];
}
inline uint64_t cell(const Mat &a, int s, int i, int j) {
  return a[(size_t)i * s + j];
}
inline Mat product(const Mat &a, const Mat &b, int s) {
  return cflcert::multiply(a, b, s);
}
inline Mat boolMat(const Mat &a) { return cflcert::booleanize(a); }
inline bool leq(const Mat &a, const Mat &b) { return cflcert::boolLeq(a, b); }

// T[A,w]: embedded |S|x|S| matrix with transitions pA -> q w,
// where p,q are PDA control states. For w=epsilon, B, or BC.
struct TransitionMatrices {
  std::vector<Mat> eps; // [A]
  std::vector<Mat> one; // [A][B]
  std::vector<Mat> two; // [A][B][C]
};
inline TransitionMatrices makeTransitionMatrices(const PDAInstance &input) {
  const PDAInstance p = normalizePAutomaton(input);
  p.validate();
  const int g = p.stackAlphabet, s = p.target.states;
  TransitionMatrices t;
  t.eps.assign(g, zeros(s));
  t.one.assign((size_t)g * g, zeros(s));
  t.two.assign((size_t)g * g * g, zeros(s));
  auto idx2 = [&](int a, int b) { return (size_t)a * g + b; };
  auto idx3 = [&](int a, int b, int c) { return ((size_t)a * g + b) * g + c; };
  for (const auto &r : p.transitions) {
    if (r.pushWord.empty())
      cell(t.eps[r.popSymbol], s, r.fromState, r.toState) = 1;
    else if (r.pushWord.size() == 1)
      cell(t.one[idx2(r.popSymbol, r.pushWord[0])], s, r.fromState, r.toState) =
          1;
    else
      cell(t.two[idx3(r.popSymbol, r.pushWord[0], r.pushWord[1])], s,
           r.fromState, r.toState) = 1;
  }
  return t;
}
inline std::vector<Mat> originalAutomatonMatrices(const PDAInstance &p) {
  const int g = p.stackAlphabet, s = p.target.states;
  std::vector<Mat> a(g, zeros(s));
  for (int u = 0; u < s; ++u)
    for (auto [x, v] : p.target.edges[u])
      cell(a[x], s, u, v) = 1;
  return a;
}

// Certificate matrices: M_A, M_{A,B}, M^1_{A,B,C}, M^2_{A,B,C}.
// Flat indices: AB=A*g+B; ABC=(A*g+B)*g+C.
struct NegativeCertificate {
  std::vector<Mat> M;    // g matrices
  std::vector<Mat> M2;   // g^2 matrices
  std::vector<Mat> M3_1; // g^3 matrices
  std::vector<Mat> M3_2; // g^3 matrices
};

inline bool verifyNegativeDeterministic(const PDAInstance &input,
                                        const NegativeCertificate &c) {
  const PDAInstance p = normalizePAutomaton(input);
  p.validate();
  const int g = p.stackAlphabet, s = p.target.states;
  if (c.M.size() != (size_t)g || c.M2.size() != (size_t)g * g ||
      c.M3_1.size() != (size_t)g * g * g || c.M3_2.size() != (size_t)g * g * g)
    return false;
  auto ix2 = [&](int a, int b) { return (size_t)a * g + b; };
  auto ix3 = [&](int a, int b, int d) { return ((size_t)a * g + b) * g + d; };
  auto T = makeTransitionMatrices(p);
  auto PA = originalAutomatonMatrices(p);
  const uint64_t maxEntry = (uint64_t)s * (uint64_t)s;
  for (const auto &v : c.M) {
    if (v.size() != (size_t)s * s)
      return false;
    for (auto x : v)
      if (x > 1)
        return false;
  }
  auto validShape = [&](const std::vector<Mat> &vv) {
    for (const auto &m : vv)
      if (m.size() != (size_t)s * s)
        return false;
    return true;
  };
  if (!validShape(c.M2) || !validShape(c.M3_1) || !validShape(c.M3_2))
    return false;
  for (const auto *vv : {&c.M2, &c.M3_1})
    for (const auto &m : *vv)
      for (auto x : m)
        if (x > (uint64_t)s)
          return false;
  for (const auto &m : c.M3_2)
    for (auto x : m)
      if (x > maxEntry)
        return false;
  // P_A <= M_A and T_{A,epsilon} <= M_A.
  for (int a = 0; a < g; ++a)
    if (!leq(PA[a], c.M[a]) || !leq(T.eps[a], c.M[a]))
      return false;
  // Exact equalities and inductiveness inclusions from Eq. (4).
  for (int a = 0; a < g; ++a) {
    for (int b = 0; b < g; ++b) {
      size_t ab = ix2(a, b);
      Mat x = product(T.one[ab], c.M[b], s);
      if (x != c.M2[ab] || !leq(boolMat(c.M2[ab]), c.M[a]))
        return false;
      for (int d = 0; d < g; ++d) {
        size_t abc = ix3(a, b, d);
        Mat y = product(T.two[abc], c.M[b], s);
        if (y != c.M3_1[abc])
          return false;
        Mat z = product(c.M3_1[abc], c.M[d], s);
        if (z != c.M3_2[abc] || !leq(boolMat(c.M3_2[abc]), c.M[a]))
          return false;
      }
    }
  }
  // Initial configuration must not be accepted by the target P-automaton
  // augmented with the invariant relation for its first stack symbol.
  for (int f : p.target.finals)
    if (cell(c.M[p.initialStackSymbol], s, p.initialState, f) != 0)
      return false;
  return true;
}

// Construct the least Boolean inductive family M_A by saturation. This is a
// certificate constructor, not the verifier; it may take many iterations.
inline NegativeCertificate buildNegativeCertificate(const PDAInstance &input) {
  const PDAInstance p = normalizePAutomaton(input);
  p.validate();
  const int g = p.stackAlphabet, s = p.target.states;
  auto T = makeTransitionMatrices(p);
  auto PA = originalAutomatonMatrices(p);
  NegativeCertificate c;
  c.M = PA;
  for (int a = 0; a < g; ++a)
    for (size_t z = 0; z < c.M[a].size(); ++z)
      if (T.eps[a][z])
        c.M[a][z] = 1;
  bool changed = true;
  while (changed) {
    changed = false;
    for (int a = 0; a < g; ++a) {
      for (int b = 0; b < g; ++b) {
        Mat x = boolMat(product(T.one[(size_t)a * g + b], c.M[b], s));
        for (size_t z = 0; z < x.size(); ++z)
          if (x[z] && !c.M[a][z])
            c.M[a][z] = 1, changed = true;
        for (int d = 0; d < g; ++d) {
          size_t abc = ((size_t)a * g + b) * g + d;
          Mat y = product(T.two[abc], c.M[b], s);
          Mat zmat = boolMat(product(y, c.M[d], s));
          for (size_t z = 0; z < zmat.size(); ++z)
            if (zmat[z] && !c.M[a][z])
              c.M[a][z] = 1, changed = true;
        }
      }
    }
  }
  c.M2.resize((size_t)g * g);
  c.M3_1.resize((size_t)g * g * g);
  c.M3_2.resize((size_t)g * g * g);
  for (int a = 0; a < g; ++a)
    for (int b = 0; b < g; ++b) {
      size_t ab = (size_t)a * g + b;
      c.M2[ab] = product(T.one[ab], c.M[b], s);
      for (int d = 0; d < g; ++d) {
        size_t abc = (ab * g) + d;
        c.M3_1[abc] = product(T.two[abc], c.M[b], s);
        c.M3_2[abc] = product(c.M3_1[abc], c.M[d], s);
      }
    }
  return c;
}

// Freivalds over nonnegative integers with 0/1 random vectors, as in the
// paper. Certificate bounds give M_{A,B} and M^1_{A,B,C} entries <= |S|,
// M^2 entries <= |S|^2, and the largest accumulator in a check is <= |S|^3.
// The guard below ensures this fits uint64_t exactly (no modular wraparound).
inline bool freivaldsProduct(const Mat &A, const Mat &B, const Mat &C, int n,
                             int repetitions, std::mt19937_64 &rng) {
  constexpr uint64_t MAX_N = 46340ULL;
  if (n <= 0 || (uint64_t)n > MAX_N || repetitions <= 0 ||
      A.size() != (size_t)n * n || B.size() != (size_t)n * n ||
      C.size() != (size_t)n * n)
    return false;
  std::uniform_int_distribution<int> bit(0, 1);
  std::vector<uint64_t> v(n), bv(n), av(n), cv(n);
  for (int r = 0; r < repetitions; ++r) {
    for (auto &x : v)
      x = (uint64_t)bit(rng);
    std::fill(bv.begin(), bv.end(), 0);
    std::fill(av.begin(), av.end(), 0);
    std::fill(cv.begin(), cv.end(), 0);
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < n; ++j) {
        bv[i] += cell(B, n, i, j) * v[j];
        cv[i] += cell(C, n, i, j) * v[j];
      }
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < n; ++j)
        av[i] += cell(A, n, i, j) * bv[j];
    if (av != cv)
      return false;
  }
  return true;
}

// Randomized checker. Four independent Freivalds repetitions are applied
// to each matrix equality. If any certificate equality is false, that false
// equality alone is detected with probability at least 15/16.
inline bool verifyNegativeRandomized(const PDAInstance &input,
                                     const NegativeCertificate &c,
                                     uint64_t seed = std::random_device{}(),
                                     int repetitionsPerProduct = 4) {
  const PDAInstance p = normalizePAutomaton(input);
  p.validate();
  const int g = p.stackAlphabet, s = p.target.states;
  if (c.M.size() != (size_t)g || c.M2.size() != (size_t)g * g ||
      c.M3_1.size() != (size_t)g * g * g || c.M3_2.size() != (size_t)g * g * g)
    return false;
  auto ix2 = [&](int a, int b) { return (size_t)a * g + b; };
  auto ix3 = [&](int a, int b, int d) { return ((size_t)a * g + b) * g + d; };
  auto T = makeTransitionMatrices(p);
  auto PA = originalAutomatonMatrices(p);
  auto shape = [&](const std::vector<Mat> &v) {
    for (const auto &m : v)
      if (m.size() != (size_t)s * s)
        return false;
    return true;
  };
  if (!shape(c.M) || !shape(c.M2) || !shape(c.M3_1) || !shape(c.M3_2))
    return false;
  if (s > 2000000)
    return false; // exact uint64 accumulator bound: s^3 < 2^64
  const uint64_t maxEntry = (uint64_t)s * (uint64_t)s;
  for (const auto &m : c.M)
    for (auto x : m)
      if (x > 1)
        return false;
  for (const auto *v : {&c.M2, &c.M3_1})
    for (const auto &m : *v)
      for (auto x : m)
        if (x > (uint64_t)s)
          return false;
  for (const auto &m : c.M3_2)
    for (auto x : m)
      if (x > maxEntry)
        return false;
  for (int a = 0; a < g; ++a)
    if (!leq(PA[a], c.M[a]) || !leq(T.eps[a], c.M[a]))
      return false;
  for (int f : p.target.finals)
    if (cell(c.M[p.initialStackSymbol], s, p.initialState, f) != 0)
      return false;
  if (repetitionsPerProduct <= 0)
    repetitionsPerProduct = 4;
  // If any equality is false, the verifier checks that particular false
  // equality with four independent Freivalds trials. Its probability of
  // escaping detection is at most 1/16; other checks can only reject more.
  // Thus no union bound over the number of matrix equations is necessary.
  std::mt19937_64 rng(seed);
  for (int a = 0; a < g; ++a)
    for (int b = 0; b < g; ++b) {
      size_t ab = ix2(a, b);
      if (!leq(boolMat(c.M2[ab]), c.M[a]))
        return false;
      if (!freivaldsProduct(T.one[ab], c.M[b], c.M2[ab], s,
                            repetitionsPerProduct, rng))
        return false;
      for (int d = 0; d < g; ++d) {
        size_t abc = ix3(a, b, d);
        if (!freivaldsProduct(T.two[abc], c.M[b], c.M3_1[abc], s,
                              repetitionsPerProduct, rng))
          return false;
        if (!freivaldsProduct(c.M3_1[abc], c.M[d], c.M3_2[abc], s,
                              repetitionsPerProduct, rng))
          return false;
        if (!leq(boolMat(c.M3_2[abc]), c.M[a]))
          return false;
      }
    }
  return true;
}

// Proposition 20 (second reduction): PDA emptiness with one initial stack
// symbol and a set of accepting control states -> Dyck-2 reachability.
// The stack alphabet is encoded by equal-length binary opening blocks.
// A transition p,A -> q,w contributes close(code(A)) followed by
// open(code(reverse(w))); the replacement word is stored top-first. The final
// path closes the initial bottom-symbol code.
struct EmptinessPDA {
  int states = 0, stackAlphabet = 0, initialState = 0, initialSymbol = 0;
  std::vector<int> acceptingStates;
  std::vector<Transition> transitions;
};
inline cflcert::Instance reducePDAEmptinessToDyck2(const EmptinessPDA &p) {
  if (p.states <= 0 || p.stackAlphabet <= 0 || p.initialState < 0 ||
      p.initialState >= p.states || p.initialSymbol < 0 ||
      p.initialSymbol >= p.stackAlphabet)
    throw std::invalid_argument("invalid PDA emptiness instance");
  for (int f : p.acceptingStates)
    if (f < 0 || f >= p.states)
      throw std::invalid_argument("invalid accepting state");
  for (const auto &r : p.transitions)
    if (r.fromState < 0 || r.fromState >= p.states || r.toState < 0 ||
        r.toState >= p.states || r.popSymbol < 0 ||
        r.popSymbol >= p.stackAlphabet)
      throw std::invalid_argument("invalid PDA transition");
  for (const auto &r : p.transitions)
    for (int a : r.pushWord)
      if (a < 0 || a >= p.stackAlphabet)
        throw std::invalid_argument("invalid pushed symbol");
  int k = 1;
  while ((1ULL << k) < (uint64_t)p.stackAlphabet)
    ++k;
  auto code = [&](int a) {
    std::vector<Label> v;
    v.reserve(k);
    for (int b = k - 1; b >= 0; --b)
      v.push_back(((a >> b) & 1) ? Label::Open2 : Label::Open1);
    return v;
  };
  std::vector<Edge> edges;
  int next = p.states;
  auto chain = [&](int from, int to, const std::vector<Label> &word) {
    int cur = from;
    for (size_t i = 0; i < word.size(); ++i) {
      int dst = (i + 1 == word.size() ? to : next++);
      edges.push_back({cur, dst, word[i]});
      cur = dst;
    }
  };
  // Initial stack symbol is pushed from the new source into q0.
  int source = next++;
  chain(source, p.initialState, code(p.initialSymbol));
  for (const auto &r : p.transitions) {
    std::vector<Label> word;
    auto a = code(r.popSymbol);
    for (auto it = a.rbegin(); it != a.rend(); ++it)
      word.push_back(*it == Label::Open1 ? Label::Close1 : Label::Close2);
    // PDA replacement words are top-first, while unmatched Dyck opens
    // represent the stack bottom-to-top. Append the reverse word, as in
    // Lemma 27's encoding of psi(A) followed by phi(gamma).
    for (auto it = r.pushWord.rbegin(); it != r.pushWord.rend(); ++it) {
      auto b = code(*it);
      word.insert(word.end(), b.begin(), b.end());
    }
    chain(r.fromState, r.toState, word);
  }
  int target = next++;
  auto bottomCode = code(p.initialSymbol);
  std::vector<Label> bottomClose;
  bottomClose.reserve(bottomCode.size());
  for (auto it = bottomCode.rbegin(); it != bottomCode.rend(); ++it)
    bottomClose.push_back(*it == Label::Open1 ? Label::Close1 : Label::Close2);
  if (bottomClose.size() == 1) {
    for (int f : p.acceptingStates)
      edges.push_back({f, target, bottomClose[0]});
  } else if (!p.acceptingStates.empty()) {
    int shared = next++;
    for (int f : p.acceptingStates)
      edges.push_back({f, shared, bottomClose[0]});
    std::vector<Label> suffix(bottomClose.begin() + 1, bottomClose.end());
    chain(shared, target, suffix);
  }
  cflcert::Instance out;
  out.n = next;
  out.source = source;
  out.target = target;
  out.edges = std::move(edges);
  out.validate();
  return out;
}

// ---------- Positive pushdown certificates (Section 5, triplet CFG) ----------
// To make the target P-automaton a genuine *final phase* (rather than mixing
// its edges with PDA transitions), form a combined PDA with two disjoint
// state sets: original PDA states Q and a copy of every P-automaton state S.
// From q in Q it may switch to the copy of q without changing the stack; in
// the copied states it can only consume stack symbols along P-automaton edges.
// The ordinary triplet grammar of this combined PDA is equivalent to the
// original reachability question and avoids treating a P-automaton state as
// an intermediate PDA control state.
struct CombinedTransition {
  int fromState = 0, popSymbol = 0, toState = 0;
  std::vector<int> pushWord; // top-first, same convention as PDAInstance
};
inline std::vector<CombinedTransition>
combinedTransitions(const PDAInstance &input) {
  const PDAInstance p = normalizePAutomaton(input);
  p.validate();
  const int Q = p.controlStates, S = p.target.states, g = p.stackAlphabet;
  std::vector<CombinedTransition> out;
  out.reserve(p.transitions.size() + (size_t)Q * g);
  for (const auto &t : p.transitions)
    out.push_back({t.fromState, t.popSymbol, t.toState, t.pushWord});
  // Switch from PDA execution to the target-automaton phase, preserving
  // the current top symbol. No transition returns from the copied states.
  for (int q = 0; q < Q; ++q)
    for (int A = 0; A < g; ++A)
      out.push_back({q, A, Q + q, {A}});
  // In the target phase, each P-automaton edge consumes one stack symbol.
  for (int u = 0; u < S; ++u)
    for (auto [A, v] : p.target.edges[u])
      out.push_back({Q + u, A, Q + v, {}});
  return out;
}
struct PositiveRule {
  enum Kind : uint8_t { Pop, OnePush, TwoPush } kind = Pop;
  int transition = -1; // index in combinedTransitions(p)
  int middle = -1;     // intermediate combined state for a two-symbol push
};
struct PositiveCertificate {
  std::vector<int> ruleOf; // [A][p][s], -1 if absent
  std::vector<PositiveRule> rules;
  int acceptingFinal = -1; // accepting state ID in the combined PDA
};
inline size_t tripletId(int A, int p, int s, int /*g*/, int S) {
  return ((size_t)A * S + p) * S + s;
}
inline bool verifyPositive(const PDAInstance &input,
                           const PositiveCertificate &c) {
  const PDAInstance p = normalizePAutomaton(input);
  p.validate();
  const int g = p.stackAlphabet, S = p.controlStates + p.target.states;
  const size_t N = (size_t)g * S * S;
  if (c.ruleOf.size() != N || c.rules.size() > N)
    return false;
  for (int ri : c.ruleOf)
    if (ri < -1)
      return false;
  // A target-automaton final state f<Q can be reached in two ways:
  // (i) the PDA empties its stack at original control state f, or
  // (ii) it switches to the copied target-automaton phase before the stack
  // is empty. Both endpoints must be accepted. For f>=Q only the copied
  // phase endpoint Q+f is meaningful.
  bool isCombinedFinal = false;
  for (int f : p.target.finals) {
    if (f < p.controlStates && c.acceptingFinal == f)
      isCombinedFinal = true;
    if (c.acceptingFinal == p.controlStates + f)
      isCombinedFinal = true;
  }
  if (!isCombinedFinal || c.acceptingFinal < 0 || c.acceptingFinal >= S)
    return false;
  const size_t root =
      tripletId(p.initialStackSymbol, p.initialState, c.acceptingFinal, g, S);
  if (c.ruleOf[root] < 0)
    return false;
  const auto trans = combinedTransitions(p);
  std::vector<std::vector<size_t>> deps(N);
  std::vector<int> indeg(N, 0);
  std::vector<char> used(c.rules.size(), 0);
  for (size_t x = 0; x < N; ++x)
    if (c.ruleOf[x] >= 0) {
      int ri = c.ruleOf[x];
      if (ri >= (int)c.rules.size() || used[ri])
        return false;
      used[ri] = 1;
      const int s = (int)(x % S), q = (int)((x / S) % S),
                A = (int)(x / ((size_t)S * S));
      const auto &r = c.rules[ri];
      auto add = [&](int B, int a, int b) {
        if (B < 0 || B >= g || a < 0 || a >= S || b < 0 || b >= S)
          return false;
        size_t y = tripletId(B, a, b, g, S);
        if (c.ruleOf[y] < 0)
          return false;
        deps[x].push_back(y);
        ++indeg[y];
        return true;
      };
      if (r.transition < 0 || r.transition >= (int)trans.size())
        return false;
      const auto &t = trans[r.transition];
      if (t.fromState != q || t.popSymbol != A)
        return false;
      if (r.kind == PositiveRule::Pop) {
        if (!t.pushWord.empty() || t.toState != s)
          return false;
      } else if (r.kind == PositiveRule::OnePush) {
        if (t.pushWord.size() != 1 || !add(t.pushWord[0], t.toState, s))
          return false;
      } else if (r.kind == PositiveRule::TwoPush) {
        if (t.pushWord.size() != 2 || r.middle < 0 || r.middle >= S ||
            !add(t.pushWord[0], t.toState, r.middle) ||
            !add(t.pushWord[1], r.middle, s))
          return false;
      } else
        return false;
    }
  for (char x : used)
    if (!x)
      return false;
  std::queue<size_t> q;
  size_t count = 0, done = 0;
  for (size_t i = 0; i < N; ++i)
    if (c.ruleOf[i] >= 0) {
      ++count;
      if (!indeg[i])
        q.push(i);
    }
  while (!q.empty()) {
    auto x = q.front();
    q.pop();
    ++done;
    for (auto y : deps[x])
      if (--indeg[y] == 0)
        q.push(y);
  }
  return done == count;
}
inline bool buildPositiveCertificate(const PDAInstance &input,
                                     PositiveCertificate &c) {
  const PDAInstance p = normalizePAutomaton(input);
  p.validate();
  const int g = p.stackAlphabet, S = p.controlStates + p.target.states;
  const size_t N = (size_t)g * S * S;
  const auto trans = combinedTransitions(p);
  c.ruleOf.assign(N, -1);
  c.rules.clear();
  c.acceptingFinal = -1;
  std::vector<char> known(N, 0);
  auto add = [&](int A, int u, int v, PositiveRule r) {
    size_t x = tripletId(A, u, v, g, S);
    if (known[x])
      return false;
    known[x] = 1;
    c.ruleOf[x] = (int)c.rules.size();
    c.rules.push_back(r);
    return true;
  };
  bool changed = true;
  while (changed) {
    changed = false;
    for (int ti = 0; ti < (int)trans.size(); ++ti) {
      const auto &t = trans[ti];
      if (t.pushWord.empty())
        changed |= add(t.popSymbol, t.fromState, t.toState,
                       PositiveRule{PositiveRule::Pop, ti, -1});
      else if (t.pushWord.size() == 1) {
        int B = t.pushWord[0];
        for (int s = 0; s < S; ++s)
          if (known[tripletId(B, t.toState, s, g, S)])
            changed |= add(t.popSymbol, t.fromState, s,
                           PositiveRule{PositiveRule::OnePush, ti, -1});
      } else if (t.pushWord.size() == 2) {
        int B = t.pushWord[0], C = t.pushWord[1];
        for (int m = 0; m < S; ++m)
          if (known[tripletId(B, t.toState, m, g, S)])
            for (int s = 0; s < S; ++s)
              if (known[tripletId(C, m, s, g, S)])
                changed |= add(t.popSymbol, t.fromState, s,
                               PositiveRule{PositiveRule::TwoPush, ti, m});
      }
    }
  }
  std::vector<int> combinedFinals;
  for (int f : p.target.finals) {
    if (f < p.controlStates)
      combinedFinals.push_back(f);                 // empty stack at original q
    combinedFinals.push_back(p.controlStates + f); // switch to target phase
  }
  std::sort(combinedFinals.begin(), combinedFinals.end());
  combinedFinals.erase(
      std::unique(combinedFinals.begin(), combinedFinals.end()),
      combinedFinals.end());
  for (int f : combinedFinals)
    if (known[tripletId(p.initialStackSymbol, p.initialState, f, g, S)]) {
      c.acceptingFinal = f;
      break;
    }
  if (c.acceptingFinal < 0)
    return false;
  const size_t root =
      tripletId(p.initialStackSymbol, p.initialState, c.acceptingFinal, g, S);
  std::vector<char> reach(N, 0);
  std::vector<size_t> st{root};
  reach[root] = 1;
  while (!st.empty()) {
    size_t x = st.back();
    st.pop_back();
    const auto &r = c.rules[c.ruleOf[x]];
    if (r.kind == PositiveRule::OnePush) {
      int s = (int)(x % S);
      const auto &t = trans[r.transition];
      size_t y = tripletId(t.pushWord[0], t.toState, s, g, S);
      if (!reach[y])
        reach[y] = 1, st.push_back(y);
    } else if (r.kind == PositiveRule::TwoPush) {
      int s = (int)(x % S);
      const auto &t = trans[r.transition];
      for (size_t y : {tripletId(t.pushWord[0], t.toState, r.middle, g, S),
                       tripletId(t.pushWord[1], r.middle, s, g, S)})
        if (!reach[y])
          reach[y] = 1, st.push_back(y);
    }
  }
  std::vector<int> remap(c.rules.size(), -1);
  std::vector<PositiveRule> compact;
  for (size_t x = 0; x < N; ++x)
    if (reach[x]) {
      int old = c.ruleOf[x];
      if (remap[old] < 0)
        remap[old] = (int)compact.size(), compact.push_back(c.rules[old]);
      c.ruleOf[x] = remap[old];
    } else
      c.ruleOf[x] = -1;
  c.rules.swap(compact);
  return verifyPositive(p, c);
}

} // namespace cflcert::pda
