#pragma once
// Section 6: two-way NPDA reductions and Theorem 21 encoding.
// The semantic hardest-language recognizer and Theorem 21 homomorphism live
// here. The explicit fixed-machine transition table is in
// HardestTwoNPDA.h (kept separate to avoid a circular dependency).
#include "CFL/Certificates/PDACertificates.h"

#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace cflcert::twopda {
struct Transition {
  int from = 0, read = 0, pop = 0, to = 0;
  std::vector<int>
      push;     // replacement word top-first order (leftmost is new top)
  int move = 0; // -1, 0, +1
};
struct Machine {
  int states = 0, alphabet = 0, stackAlphabet = 0;
  int initial = 0, bottom = 0, leftMarker = 0, rightMarker = 0;
  std::vector<int> finals;
  std::vector<Transition> transitions;
  void validate() const {
    if (states <= 0 || alphabet <= 1 || stackAlphabet <= 0 || initial < 0 ||
        initial >= states || bottom < 0 || bottom >= stackAlphabet ||
        leftMarker < 0 || leftMarker >= alphabet || rightMarker < 0 ||
        rightMarker >= alphabet || leftMarker == rightMarker)
      throw std::invalid_argument("invalid 2NPDA dimensions");
    for (int f : finals)
      if (f < 0 || f >= states)
        throw std::invalid_argument("bad final state");
    for (const auto &t : transitions) {
      if (t.from < 0 || t.from >= states || t.to < 0 || t.to >= states ||
          t.read < 0 || t.read >= alphabet || t.pop < 0 ||
          t.pop >= stackAlphabet || t.move < -1 || t.move > 1)
        throw std::invalid_argument("bad 2NPDA transition");
      for (int x : t.push)
        if (x < 0 || x >= stackAlphabet)
          throw std::invalid_argument("bad pushed symbol");
      // Transition words are top-first. The bottom marker is the final
      // symbol; a transition on it may add symbols above it but must
      // retain exactly one bottom marker at the end.
      if (t.pop == bottom) {
        if (t.push.empty() || t.push.back() != bottom ||
            std::count(t.push.begin(), t.push.end(), bottom) != 1)
          throw std::invalid_argument(
              "bottom marker must remain exactly once at stack bottom");
      } else if (std::find(t.push.begin(), t.push.end(), bottom) !=
                 t.push.end())
        throw std::invalid_argument("bottom marker may not be pushed");
    }
  }
};

// Lemma 26: product of a fixed input tape and a 2NPDA is an input-free PDA.
inline cflcert::pda::EmptinessPDA
compileInputToPDAEmptiness(const Machine &A, const std::vector<int> &word) {
  A.validate();
  if (word.size() > static_cast<size_t>(std::numeric_limits<int>::max() - 2))
    throw std::length_error("input word too long");
  for (int x : word)
    if (x < 0 || x >= A.alphabet || x == A.leftMarker || x == A.rightMarker)
      throw std::invalid_argument("word contains invalid symbol/endmarker");
  const int m = (int)word.size(), Q = A.states;
  if ((long long)(m + 2) * Q > std::numeric_limits<int>::max())
    throw std::length_error("2NPDA/input product too large");
  std::vector<int> tape;
  tape.reserve(m + 2);
  tape.push_back(A.leftMarker);
  tape.insert(tape.end(), word.begin(), word.end());
  tape.push_back(A.rightMarker);
  cflcert::pda::EmptinessPDA P;
  P.states = (m + 2) * Q;
  P.stackAlphabet = A.stackAlphabet;
  P.initialState = A.initial;
  P.initialSymbol = A.bottom;
  auto state = [&](int pos, int q) { return pos * Q + q; };
  for (int f : A.finals)
    P.acceptingStates.push_back(state(m + 1, f));
  for (int pos = 0; pos < m + 2; ++pos)
    for (const auto &t : A.transitions)
      if (t.read == tape[pos]) {
        int np = pos + t.move;
        if (np < 0 || np >= m + 2)
          continue;
        P.transitions.push_back(
            {state(pos, t.from), t.pop, state(np, t.to), t.push});
      }
  return P;
}

inline std::string offsetWord(int d) {
  if (d == 0)
    return {};
  const int64_t value = d;
  const uint64_t magnitude = static_cast<uint64_t>(value < 0 ? -value : value);
  std::string s;
  if (value < 0)
    s.push_back('-');
  s.append(static_cast<size_t>(magnitude), '1');
  return s;
}
struct EncodedEdge {
  char label = '(';
  int offset = 0;
};
using EncodedGraph = std::vector<std::vector<EncodedEdge>>;
inline std::string encodeGraph(const EncodedGraph &g) {
  std::string out;
  for (const auto &v : g) {
    out.push_back('#');
    for (size_t i = 0; i < v.size(); ++i) {
      if (i)
        out.push_back('*');
      out.push_back(v[i].label);
      out += offsetWord(v[i].offset);
    }
  }
  return out;
}

enum class Decision { Reject, Accept, Invalid };

// Lemma 28: a binary-ID encoding of an arbitrary D2Reach instance.
// Format: bin(s);bin(t); then zero or more records
// bin(u),bin(v),label; where label is one of ()[]. IDs are unsigned binary
// integers without leading zeroes. The parser coordinate-compresses IDs.
inline std::string binaryId(uint64_t x) {
  if (x == 0)
    return "0";
  std::string s;
  while (x) {
    s.push_back((x & 1) ? '1' : '0');
    x >>= 1;
  }
  std::reverse(s.begin(), s.end());
  return s;
}
inline bool parseBinaryId(const std::string &w, size_t &pos, uint64_t &value,
                          char terminator) {
  if (pos >= w.size())
    return false;
  size_t begin = pos;
  if (w[pos] != '0' && w[pos] != '1')
    return false;
  if (w[pos] == '0' && pos + 1 < w.size() && w[pos + 1] != terminator)
    return false;
  value = 0;
  while (pos < w.size() && (w[pos] == '0' || w[pos] == '1')) {
    if (value > (std::numeric_limits<uint64_t>::max() >> 1))
      return false;
    value = (value << 1) | (uint64_t)(w[pos] - '0');
    ++pos;
  }
  return pos > begin && pos < w.size() && w[pos] == terminator;
}
inline std::string encodeGeneralD2Instance(const cflcert::Instance &g) {
  g.validate();
  std::string out =
      binaryId((uint64_t)g.source) + ";" + binaryId((uint64_t)g.target) + ";";
  for (const auto &e : g.edges) {
    out += binaryId((uint64_t)e.from);
    out.push_back(',');
    out += binaryId((uint64_t)e.to);
    out.push_back(',');
    out.push_back(e.label == cflcert::Label::Open1    ? '('
                  : e.label == cflcert::Label::Close1 ? ')'
                  : e.label == cflcert::Label::Open2  ? '['
                                                      : ']');
    out.push_back(';');
  }
  return out;
}
inline bool parseGeneralD2Instance(const std::string &w,
                                   cflcert::Instance &out) {
  size_t p = 0;
  uint64_t src = 0, dst = 0;
  if (!parseBinaryId(w, p, src, ';'))
    return false;
  ++p;
  if (!parseBinaryId(w, p, dst, ';'))
    return false;
  ++p;
  std::vector<std::tuple<uint64_t, uint64_t, cflcert::Label>> edges;
  std::vector<uint64_t> ids{src, dst};
  while (p < w.size()) {
    uint64_t u = 0, v = 0;
    if (!parseBinaryId(w, p, u, ','))
      return false;
    ++p;
    if (!parseBinaryId(w, p, v, ','))
      return false;
    ++p;
    if (p >= w.size())
      return false;
    char l = w[p++];
    cflcert::Label label;
    if (l == '(')
      label = cflcert::Label::Open1;
    else if (l == ')')
      label = cflcert::Label::Close1;
    else if (l == '[')
      label = cflcert::Label::Open2;
    else if (l == ']')
      label = cflcert::Label::Close2;
    else
      return false;
    if (p >= w.size() || w[p++] != ';')
      return false;
    edges.emplace_back(u, v, label);
    ids.push_back(u);
    ids.push_back(v);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (ids.empty() || ids.size() > 46340)
    return false;
  auto index = [&](uint64_t x) {
    return (int)(std::lower_bound(ids.begin(), ids.end(), x) - ids.begin());
  };
  out.n = (int)ids.size();
  out.source = index(src);
  out.target = index(dst);
  out.edges.clear();
  for (const auto &e : edges)
    out.edges.push_back(
        {index(std::get<0>(e)), index(std::get<1>(e)), std::get<2>(e)});
  out.validate();
  return true;
}
// Exact semantic interpreter for the fixed 2NPDA of Lemma 28: it validates
// the encoding, then decides whether the encoded graph has a Dyck-2 path.
inline Decision recognizeGeneralD2Language(const std::string &word) {
  cflcert::Instance g;
  if (!parseGeneralD2Instance(word, g))
    return Decision::Invalid;
  return cflcert::decideCubic(g) ? Decision::Accept : Decision::Reject;
}

// Parser for the fixed hardest-language format (5).
inline bool parseHardestWord(const std::string &w, EncodedGraph &g) {
  g.clear();
  size_t i = 0;
  if (w.empty())
    return false;
  while (i < w.size()) {
    if (w[i++] != '#')
      return false;
    std::vector<EncodedEdge> edges;
    while (i < w.size() && w[i] != '#') {
      char l = w[i++];
      if (l != '(' && l != ')' && l != '[' && l != ']')
        return false;
      int sign = 1;
      if (i < w.size() && w[i] == '-') {
        sign = -1;
        ++i;
      }
      int count = 0;
      while (i < w.size() && w[i] == '1') {
        if (count == std::numeric_limits<int>::max())
          return false;
        ++count;
        ++i;
      }
      if (sign < 0 && count == 0)
        return false;
      if (i < w.size() && w[i] != '*' && w[i] != '#')
        return false;
      edges.push_back({l, sign * count});
      if (i < w.size() && w[i] == '*') {
        ++i;
        if (i >= w.size() || w[i] == '#')
          return false;
      } else
        break;
    }
    if (g.size() >= static_cast<size_t>(std::numeric_limits<int>::max()))
      return false;
    g.push_back(std::move(edges));
  }
  return !g.empty();
}
inline int reflectedDestination(int source, int offset, int n) {
  if (n <= 0 || source < 1 || source > n)
    throw std::invalid_argument("invalid reflected-walk coordinates");
  // Unfold the reflected walk onto a line of period 2(n+1). This is
  // equivalent to stepping one cell at a time and reversing direction
  // whenever a virtual endmarker (0 or n+1) is reached.
  const int64_t period = 2LL * (static_cast<int64_t>(n) + 1);
  int64_t y =
      (static_cast<int64_t>(source) + static_cast<int64_t>(offset)) % period;
  if (y < 0)
    y += period;
  if (y > n + 1)
    y = period - y;
  return static_cast<int>(y);
}
// Exact decider for the fixed language L(A0), using the equivalent D2Reach
// closure. This is a language interpreter, not a time-bounded NPDA run.
inline Decision recognizeHardestLanguage(const std::string &w) {
  EncodedGraph enc;
  if (!parseHardestWord(w, enc))
    return Decision::Invalid;
  if (enc.size() > 46338)
    return Decision::Invalid;
  int n = (int)enc.size();
  cflcert::Instance g;
  g.n = n;
  g.source = 0;
  g.target = n - 1;
  for (int u = 1; u <= n; ++u)
    for (const auto &e : enc[u - 1]) {
      int v = reflectedDestination(u, e.offset, n);
      // The endmarkers are virtual positions used while counting offsets,
      // not vertices of G. An edge ending exactly on one is not an edge.
      if (v < 1 || v > n)
        continue;
      cflcert::Label l = e.label == '('   ? cflcert::Label::Open1
                         : e.label == ')' ? cflcert::Label::Close1
                         : e.label == '[' ? cflcert::Label::Open2
                                          : cflcert::Label::Close2;
      g.edges.push_back({u - 1, v - 1, l});
    }
  cflcert::Separator s;
  cflcert::buildSeparator(g, s);
  return cflcert::at(s.S, g.n, g.source, g.target) ? Decision::Accept
                                                   : Decision::Reject;
}

// Bounded operational simulation of the fixed 2NPDA. A configuration stores
// the current vertex and the unmatched opening-parenthesis stack; the actual
// 2NPDA stores the vertex encoding above that stack and scans the edge list.
// Because nondeterministic runs can be infinite, the simulator reports
// Inconclusive when its explicit configuration budget is exhausted.
enum class Simulation { Reject, Accept, Inconclusive, Invalid };
struct RunConfiguration {
  int vertex = 1;
  std::vector<char> stack;
};
inline Simulation simulateHardest2NPDA(const std::string &w,
                                       size_t configurationBudget = 100000) {
  EncodedGraph enc;
  if (!parseHardestWord(w, enc) ||
      enc.size() > static_cast<size_t>(std::numeric_limits<int>::max() - 2))
    return Simulation::Invalid;
  int n = (int)enc.size();
  std::vector<std::vector<std::pair<int, char>>> adj(n + 1);
  for (int u = 1; u <= n; ++u)
    for (const auto &e : enc[u - 1]) {
      int v = reflectedDestination(u, e.offset, n);
      if (v >= 1 && v <= n)
        adj[u].push_back({v, e.label});
    }
  std::queue<RunConfiguration> q;
  q.push({1, {}});
  std::set<std::string> seen;
  size_t explored = 0;
  auto key = [](const RunConfiguration &c) {
    std::string s = std::to_string(c.vertex) + ":";
    s.append(c.stack.begin(), c.stack.end());
    return s;
  };
  while (!q.empty()) {
    if (++explored > configurationBudget)
      return Simulation::Inconclusive;
    auto c = std::move(q.front());
    q.pop();
    if (c.vertex == n && c.stack.empty())
      return Simulation::Accept;
    for (auto [v, l] : adj[c.vertex]) {
      RunConfiguration d = c;
      d.vertex = v;
      if (l == '(' || l == '[')
        d.stack.push_back(l);
      else {
        char want = (l == ')' ? '(' : '[');
        if (d.stack.empty() || d.stack.back() != want)
          continue;
        d.stack.pop_back();
      }
      std::string k = key(d);
      if (seen.insert(k).second)
        q.push(std::move(d));
    }
  }
  return Simulation::Reject;
}

// Theorem 21 homomorphism generator. It compiles A's input-position product,
// then the PDA->Dyck graph gadgets, then the three-track shuffle. The returned
// map is a monoid homomorphism: h(w1...wk)=h(w1)...h(wk).
class Theorem21Homomorphism {
  Machine A;
  int ell = 1, K = 0, Q = 0;
  std::vector<cflcert::Label> phi(int z) const {
    std::vector<cflcert::Label> x;
    for (int b = ell - 1; b >= 0; --b)
      x.push_back((z >> b) & 1 ? cflcert::Label::Open2 : cflcert::Label::Open1);
    return x;
  }
  std::vector<cflcert::Label> psi(int z) const {
    auto x = phi(z);
    std::reverse(x.begin(), x.end());
    for (auto &v : x)
      v = v == cflcert::Label::Open1 ? cflcert::Label::Close1
                                     : cflcert::Label::Close2;
    return x;
  }
  static char ch(cflcert::Label x) {
    return x == cflcert::Label::Open1    ? '('
           : x == cflcert::Label::Close1 ? ')'
           : x == cflcert::Label::Open2  ? '['
                                         : ']';
  }
  struct Arc {
    int from, toDelta, to;
    cflcert::Label label;
  };
  struct Template {
    std::vector<std::vector<Arc>> out;
  };
  std::vector<std::vector<int>> finalMid;
  std::vector<int> qLocal;
  int sourceNode = 0, targetNode = 0;
  std::vector<std::vector<int>> transMid;
  Template makeTemplate(int symbol) const {
    Template T;
    T.out.resize(K);
    auto addPath = [&](int start, int delta, int finish,
                       const std::vector<cflcert::Label> &word,
                       const std::vector<int> &mid) {
      int cur = start;
      for (size_t z = 0; z < word.size(); ++z) {
        bool last = (z + 1 == word.size());
        int nxt = (last ? finish : mid[z]);
        T.out[cur].push_back({cur, last ? delta : 0, nxt, word[z]});
        cur = nxt;
      }
    };
    if (symbol == A.leftMarker)
      addPath(sourceNode, 0, qLocal[A.initial], phi(A.bottom),
              transMid[A.transitions.size()]);
    if (symbol == A.rightMarker)
      for (size_t fi = 0; fi < A.finals.size(); ++fi)
        addPath(qLocal[A.finals[fi]], 0, targetNode, psi(A.bottom),
                finalMid[fi]);
    for (size_t ti = 0; ti < A.transitions.size(); ++ti) {
      const auto &t = A.transitions[ti];
      if (t.read != symbol)
        continue;
      if (symbol == A.leftMarker && t.move < 0)
        continue;
      if (symbol == A.rightMarker && t.move > 0)
        continue;
      std::vector<cflcert::Label> w = psi(t.pop);
      for (auto it = t.push.rbegin(); it != t.push.rend(); ++it) {
        auto z = phi(*it);
        w.insert(w.end(), z.begin(), z.end());
      }
      int dest = qLocal[t.to];
      addPath(qLocal[t.from], t.move, dest, w, transMid[ti]);
    }
    return T;
  }
  std::string encodeTriple(const Template &E, const Template &M,
                           const Template &D) const {
    std::string out;
    for (int j = 0; j < K; ++j) {
      int ej = K - 1 - j, dj = K - 1 - j;
      out.push_back('#');
      auto emit = [&](const std::vector<Arc> &arcs, int track, int local) {
        bool first = true;
        for (const auto &a : arcs) {
          if (a.from != local)
            throw std::logic_error("template arc/source index mismatch");
          int64_t off64 = 0;
          if (track == 0) { // reversed left-endmarker track
            if (a.toDelta == 0)
              off64 = 3LL * (local - a.to);
            else if (a.toDelta == 1)
              off64 = 3LL * a.to + 1 - 3LL * (K - 1 - local);
            else
              throw std::logic_error("invalid left-endmarker head movement");
          } else if (track == 1) {
            off64 = 3LL * K * a.toDelta + 3LL * (a.to - local);
          } else {
            if (a.toDelta == 0)
              off64 = 3LL * (local - a.to);
            else if (a.toDelta == -1)
              off64 = 3LL * a.to + 1 - (3LL * (K - 1 - local) + 2);
            else
              throw std::logic_error("invalid right-endmarker head movement");
          }
          if (off64 < std::numeric_limits<int>::min() ||
              off64 > std::numeric_limits<int>::max())
            throw std::length_error("Theorem 21 offset overflow");
          if (!first)
            out.push_back('*');
          first = false;
          out.push_back(ch(a.label));
          out += offsetWord(static_cast<int>(off64));
        }
      };
      emit(E.out[ej], 0, ej);
      out.push_back('#');
      emit(M.out[j], 1, j);
      out.push_back('#');
      emit(D.out[dj], 2, dj);
    }
    return out;
  }
  static Machine normalize(const Machine &input) {
    input.validate();
    Machine a = input;
    const int oldQ = input.states, newStart = oldQ, newFinal = oldQ + 1;
    a.states = oldQ + 2;
    a.initial = newStart;
    a.finals = {newFinal};
    // The initial configuration scans the left endmarker and has only
    // the distinguished bottom symbol on the stack.
    a.transitions.push_back(
        {newStart, a.leftMarker, a.bottom, input.initial, {a.bottom}, 0});
    for (int f : input.finals)
      a.transitions.push_back(
          {f, a.rightMarker, a.bottom, newFinal, {a.bottom}, 0});
    a.validate();
    return a;
  }

public:
  explicit Theorem21Homomorphism(const Machine &machine)
      : A(normalize(machine)) {
    Q = A.states;
    while ((1ULL << ell) < (uint64_t)A.stackAlphabet)
      ++ell;
    // Target state first, initial state last: reversal of the two end tracks
    // makes the source the first vertex and sink the last.
    qLocal.assign(Q, 0);
    int rank = 1;
    for (int q = 0; q < Q; ++q)
      if (std::find(A.finals.begin(), A.finals.end(), q) == A.finals.end() &&
          q != A.initial)
        qLocal[q] = rank++;
    for (int f : A.finals)
      qLocal[f] = rank++;
    qLocal[A.initial] = rank++;
    int next = 1 + Q;
    transMid.resize(A.transitions.size() + 2);
    for (size_t i = 0; i < A.transitions.size(); ++i) {
      size_t L = (size_t)ell * (1 + A.transitions[i].push.size());
      for (size_t j = 0; j + 1 < L; ++j)
        transMid[i].push_back(next++);
    }
    for (int j = 0; j < ell - 1; ++j)
      transMid[A.transitions.size()].push_back(next++); // source path
    for (size_t fi = 0; fi < A.finals.size(); ++fi) {
      finalMid.emplace_back();
      for (int j = 0; j < ell - 1; ++j)
        finalMid.back().push_back(next++);
    } // one target path per final state
    sourceNode = next++;
    targetNode = 0;
    K = next;
    if (K <= 0 || K > std::numeric_limits<int>::max() / 3)
      throw std::length_error("Theorem 21 block too large");
    // Source is placed last and target first in the unreversed templates.
    if (sourceNode != K - 1)
      throw std::logic_error("source slot ordering");
  }
  std::string image(int ordinarySymbol) const {
    if (ordinarySymbol == A.leftMarker || ordinarySymbol == A.rightMarker ||
        ordinarySymbol < 0 || ordinarySymbol >= A.alphabet)
      throw std::invalid_argument("image requires ordinary input symbol");
    auto E = makeTemplate(A.leftMarker), M = makeTemplate(ordinarySymbol),
         D = makeTemplate(A.rightMarker);
    return encodeTriple(E, M, D);
  }
  std::map<int, std::string> images() const {
    std::map<int, std::string> h;
    for (int a = 0; a < A.alphabet; ++a)
      if (a != A.leftMarker && a != A.rightMarker)
        h[a] = image(a);
    return h;
  }
  std::string apply(const std::vector<int> &w) const {
    std::string out;
    auto h = images();
    for (int a : w) {
      auto it = h.find(a);
      if (it == h.end())
        throw std::invalid_argument("input contains endmarker/unknown symbol");
      out += it->second;
    }
    return out;
  }
  int verticesPerInputSymbol() const { return 3 * K; }
};

} // namespace cflcert::twopda
