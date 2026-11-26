#pragma once
// Generic fixed-CFG reachability certificates (paper, Remark 7 and Remark 12).
// Grammar is in binary normal form: A->BC, A->a, and A->epsilon.
#include "CFL/Certificates/DyckCertificates.h"

#include <queue>
#include <stdexcept>

namespace cflcert::cfg {
struct Production {
  enum Kind { Epsilon, Terminal, Binary } kind = Epsilon;
  int lhs = 0, a = -1, b = -1, terminal = -1;
};
struct Grammar {
  int nonterminals = 0, terminals = 0, start = 0;
  std::vector<Production> productions;
  void validate() const {
    if (nonterminals <= 0 || terminals < 0 || start < 0 ||
        start >= nonterminals)
      throw std::invalid_argument("invalid CFG dimensions");
    for (const auto &p : productions) {
      if (static_cast<int>(p.kind) < static_cast<int>(Production::Epsilon) ||
          static_cast<int>(p.kind) > static_cast<int>(Production::Binary))
        throw std::invalid_argument("unknown production kind");
      if (p.lhs < 0 || p.lhs >= nonterminals)
        throw std::invalid_argument("bad production lhs");
      if (p.kind == Production::Binary &&
          (p.a < 0 || p.a >= nonterminals || p.b < 0 || p.b >= nonterminals))
        throw std::invalid_argument("bad binary production");
      if (p.kind == Production::Terminal &&
          (p.terminal < 0 || p.terminal >= terminals))
        throw std::invalid_argument("bad terminal production");
    }
  }
};
// Unnormalized CFG interface. A symbol is either a terminal or nonterminal;
// rhs is written left-to-right. normalize() preserves the language of every
// original nonterminal and emits the binary grammar accepted above.
struct Symbol {
  bool terminal = false;
  int id = 0;
  static Symbol T(int x) { return Symbol{true, x}; }
  static Symbol N(int x) { return Symbol{false, x}; }
};
struct GeneralProduction {
  int lhs = 0;
  std::vector<Symbol> rhs;
};
struct GeneralGrammar {
  int nonterminals = 0, terminals = 0, start = 0;
  std::vector<GeneralProduction> productions;
  void validate() const {
    if (nonterminals <= 0 || terminals < 0 || start < 0 ||
        start >= nonterminals)
      throw std::invalid_argument("invalid general CFG dimensions");
    for (const auto &p : productions) {
      if (p.lhs < 0 || p.lhs >= nonterminals)
        throw std::invalid_argument("bad general CFG lhs");
      for (const auto &x : p.rhs)
        if (x.id < 0 || (x.terminal ? x.id >= terminals : x.id >= nonterminals))
          throw std::invalid_argument("bad general CFG symbol");
    }
  }
};

inline Grammar normalize(const GeneralGrammar &input) {
  input.validate();
  using RHS = std::vector<Symbol>;
  int N = input.nonterminals;
  auto addNT = [&]() {
    if (N == std::numeric_limits<int>::max())
      throw std::length_error("too many CFG nonterminals");
    return N++;
  };

  // First binarize long right-hand sides. Doing this before nullable
  // elimination is important: nullable expansion then creates at most
  // four variants per production, rather than exponentially many variants
  // for one long RHS.
  std::vector<GeneralProduction> ps;
  for (const auto &p : input.productions) {
    if (p.rhs.size() <= 2) {
      ps.push_back(p);
      continue;
    }
    int lhs = p.lhs;
    for (size_t i = 0; i + 2 < p.rhs.size(); ++i) {
      int helper = addNT();
      ps.push_back({lhs, {p.rhs[i], Symbol::N(helper)}});
      lhs = helper;
    }
    ps.push_back({lhs, {p.rhs[p.rhs.size() - 2], p.rhs.back()}});
  }

  // Nullable fixed point on the binarized grammar.
  std::vector<char> nullable(N, 0);
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &p : ps) {
      bool all = true;
      for (const auto &x : p.rhs)
        if (x.terminal || !nullable[x.id]) {
          all = false;
          break;
        }
      if (all && !nullable[p.lhs])
        nullable[p.lhs] = 1, changed = true;
    }
  }

  // Eliminate epsilon productions while preserving the language of every
  // nonterminal. With binary RHSs, at most 2^2 variants are generated.
  std::vector<GeneralProduction> noEps;
  for (const auto &p : ps) {
    std::vector<RHS> variants(1);
    for (const auto &x : p.rhs) {
      std::vector<RHS> next;
      next.reserve(variants.size() * (x.terminal || !nullable[x.id] ? 1 : 2));
      for (const auto &v : variants) {
        RHS keep = v;
        keep.push_back(x);
        next.push_back(std::move(keep));
        if (!x.terminal && nullable[x.id])
          next.push_back(v);
      }
      variants.swap(next);
    }
    for (auto &rhs : variants)
      if (!rhs.empty())
        noEps.push_back({p.lhs, std::move(rhs)});
  }
  for (int a = 0; a < N; ++a)
    if (nullable[a])
      noEps.push_back({a, {}});
  ps.swap(noEps);

  // Eliminate unit productions by computing the unit closure and copying
  // all non-unit productions reachable from each lhs.
  std::vector<std::vector<int>> unit(N);
  for (const auto &p : ps)
    if (p.rhs.size() == 1 && !p.rhs[0].terminal)
      unit[p.lhs].push_back(p.rhs[0].id);
  std::vector<GeneralProduction> nonunit;
  for (const auto &p : ps)
    if (!(p.rhs.size() == 1 && !p.rhs[0].terminal))
      nonunit.push_back(p);
  std::vector<GeneralProduction> noUnit;
  for (int a = 0; a < N; ++a) {
    std::vector<char> seen(N, 0);
    std::vector<int> st{a};
    seen[a] = 1;
    while (!st.empty()) {
      int x = st.back();
      st.pop_back();
      for (int y : unit[x])
        if (!seen[y])
          seen[y] = 1, st.push_back(y);
    }
    for (int x = 0; x < N; ++x)
      if (seen[x])
        for (const auto &p : nonunit)
          if (p.lhs == x)
            noUnit.push_back({a, p.rhs});
  }
  ps.swap(noUnit);

  // Isolate terminals in binary productions. Single-terminal productions
  // remain A -> a; binary productions become A -> B C.
  std::vector<int> terminalVar(input.terminals, -1);
  std::vector<GeneralProduction> isolated;
  for (const auto &p : ps) {
    RHS rhs = p.rhs;
    if (rhs.size() == 2)
      for (auto &x : rhs)
        if (x.terminal) {
          if (terminalVar[x.id] < 0) {
            terminalVar[x.id] = addNT();
            isolated.push_back({terminalVar[x.id], {x}});
          }
          x = Symbol::N(terminalVar[x.id]);
        }
    isolated.push_back({p.lhs, std::move(rhs)});
  }
  ps.swap(isolated);

  Grammar out;
  out.nonterminals = N;
  out.terminals = input.terminals;
  out.start = input.start;
  for (const auto &p : ps) {
    if (p.rhs.empty())
      out.productions.push_back({Production::Epsilon, p.lhs});
    else if (p.rhs.size() == 1) {
      if (!p.rhs[0].terminal)
        throw std::logic_error("unit production survived normalization");
      out.productions.push_back(
          {Production::Terminal, p.lhs, -1, -1, p.rhs[0].id});
    } else {
      if (p.rhs.size() != 2 || p.rhs[0].terminal || p.rhs[1].terminal)
        throw std::logic_error(
            "CFG normalization did not reach binary normal form");
      out.productions.push_back(
          {Production::Binary, p.lhs, p.rhs[0].id, p.rhs[1].id});
    }
  }
  out.validate();
  return out;
}

struct CfgEdge {
  int from = 0, to = 0, label = 0;
};
struct Graph {
  int n = 0, source = 0, target = 0;
  std::vector<CfgEdge> edges;
};
struct Rule {
  int production = -1, split = -1, edge = -1;
};
struct PositiveCertificate {
  // Rule for (A,u,v), flattened ((A*n+u)*n+v), -1 means absent.
  std::vector<int> ruleOf;
  std::vector<Rule> rules;
};
inline size_t id(int A, int u, int v, int n) {
  return ((size_t)A * n + u) * n + v;
}
inline bool verifyPositive(const Grammar &gr, const Graph &g,
                           const PositiveCertificate &c) {
  gr.validate();
  if (g.n <= 0 || g.source < 0 || g.source >= g.n || g.target < 0 ||
      g.target >= g.n)
    return false;
  size_t N = (size_t)gr.nonterminals * g.n * g.n;
  if (c.ruleOf.size() != N || c.rules.size() > N)
    return false;
  for (int ri : c.ruleOf)
    if (ri < -1)
      return false;
  std::vector<std::vector<size_t>> deps(N);
  std::vector<int> indeg(N, 0);
  std::vector<char> used(c.rules.size(), 0);
  size_t root = id(gr.start, g.source, g.target, g.n);
  if (c.ruleOf[root] < 0)
    return false;
  for (const auto &e : g.edges)
    if (e.from < 0 || e.from >= g.n || e.to < 0 || e.to >= g.n || e.label < 0 ||
        e.label >= gr.terminals)
      return false;
  for (size_t x = 0; x < N; ++x)
    if (c.ruleOf[x] >= 0) {
      int ri = c.ruleOf[x];
      if (ri >= (int)c.rules.size() || used[ri])
        return false;
      used[ri] = 1;
      int v = x % g.n, u = (x / g.n) % g.n;
      const auto &r = c.rules[ri];
      if (r.production < 0 || r.production >= (int)gr.productions.size())
        return false;
      const auto &p = gr.productions[r.production];
      // A production selected for [A,u,v] must have A on its left-hand side.
      if (p.lhs != static_cast<int>(x / ((size_t)g.n * g.n)))
        return false;
      auto dep = [&](int B, int a, int b) {
        size_t y = id(B, a, b, g.n);
        if (c.ruleOf[y] < 0)
          return false;
        deps[x].push_back(y);
        ++indeg[y];
        return true;
      };
      if (p.kind == Production::Epsilon) {
        if (u != v)
          return false;
      } else if (p.kind == Production::Terminal) {
        if (r.edge < 0 || r.edge >= (int)g.edges.size())
          return false;
        const auto &e = g.edges[r.edge];
        if (e.from != u || e.to != v || e.label != p.terminal)
          return false;
      } else {
        if (r.split < 0 || r.split >= g.n || !dep(p.a, u, r.split) ||
            !dep(p.b, r.split, v))
          return false;
      }
    }
  for (char x : used)
    if (!x)
      return false;
  std::queue<size_t> q;
  size_t count = 0;
  for (size_t i = 0; i < N; ++i)
    if (c.ruleOf[i] >= 0) {
      ++count;
      if (indeg[i] == 0)
        q.push(i);
    }
  size_t done = 0;
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
// Saturation constructor. The first discovered derivation is acyclic by
// construction.
inline bool buildPositive(const Grammar &gr, const Graph &g,
                          PositiveCertificate &out) {
  gr.validate();
  if (g.n <= 0 || g.source < 0 || g.source >= g.n || g.target < 0 ||
      g.target >= g.n)
    throw std::invalid_argument("bad graph");
  for (auto e : g.edges)
    if (e.from < 0 || e.from >= g.n || e.to < 0 || e.to >= g.n || e.label < 0 ||
        e.label >= gr.terminals)
      throw std::invalid_argument("bad graph edge");
  size_t N = (size_t)gr.nonterminals * g.n * g.n;
  out.ruleOf.assign(N, -1);
  out.rules.clear();
  std::vector<char> known(N, 0);
  auto add = [&](int A, int u, int v, Rule r) {
    size_t x = id(A, u, v, g.n);
    if (known[x])
      return false;
    known[x] = 1;
    out.ruleOf[x] = (int)out.rules.size();
    out.rules.push_back(r);
    return true;
  };
  bool change = true;
  while (change) {
    change = false;
    for (int pi = 0; pi < (int)gr.productions.size(); ++pi) {
      const auto &p = gr.productions[pi];
      if (p.kind == Production::Epsilon)
        for (int u = 0; u < g.n; ++u)
          change |= add(p.lhs, u, u, Rule{pi, -1, -1});
      else if (p.kind == Production::Terminal)
        for (int ei = 0; ei < (int)g.edges.size(); ++ei) {
          const auto &e = g.edges[ei];
          if (e.label == p.terminal)
            change |= add(p.lhs, e.from, e.to, Rule{pi, -1, ei});
        }
    }
    for (int pi = 0; pi < (int)gr.productions.size(); ++pi) {
      const auto &p = gr.productions[pi];
      if (p.kind != Production::Binary)
        continue;
      for (int u = 0; u < g.n; ++u)
        for (int w = 0; w < g.n; ++w)
          if (known[id(p.a, u, w, g.n)])
            for (int v = 0; v < g.n; ++v)
              if (known[id(p.b, w, v, g.n)])
                change |= add(p.lhs, u, v, Rule{pi, w, -1});
    }
  }
  size_t root = id(gr.start, g.source, g.target, g.n);
  if (!known[root])
    return false;
  // Retain only dependencies reachable from the axiom, then compact rules.
  std::vector<char> reach(N, 0);
  std::vector<size_t> st{root};
  reach[root] = 1;
  while (!st.empty()) {
    size_t x = st.back();
    st.pop_back();
    int ri = out.ruleOf[x];
    const auto &r = out.rules[ri];
    const auto &p = gr.productions[r.production];
    if (p.kind == Production::Binary) {
      int v = x % g.n, u = (x / g.n) % g.n;
      for (auto y : {id(p.a, u, r.split, g.n), id(p.b, r.split, v, g.n)})
        if (!reach[y])
          reach[y] = 1, st.push_back(y);
    }
  }
  std::vector<int> remap(out.rules.size(), -1);
  std::vector<Rule> compact;
  for (size_t x = 0; x < N; ++x)
    if (reach[x]) {
      int old = out.ruleOf[x];
      if (remap[old] < 0)
        remap[old] = (int)compact.size(), compact.push_back(out.rules[old]);
      out.ruleOf[x] = remap[old];
    } else
      out.ruleOf[x] = -1;
  out.rules.swap(compact);
  return verifyPositive(gr, g, out);
}

struct NegativeCertificate {
  std::vector<Matrix> M;
  std::vector<Matrix> productByProduction;
};
inline bool verifyNegative(const Grammar &gr, const Graph &g,
                           const NegativeCertificate &c) {
  gr.validate();
  if (g.n <= 0 || g.source < 0 || g.source >= g.n || g.target < 0 ||
      g.target >= g.n)
    return false;
  int k = gr.nonterminals, n = g.n;
  if (c.M.size() != (size_t)k ||
      c.productByProduction.size() != gr.productions.size())
    return false;
  for (auto e : g.edges)
    if (e.from < 0 || e.from >= n || e.to < 0 || e.to >= n || e.label < 0 ||
        e.label >= gr.terminals)
      return false;
  for (auto &m : c.M) {
    if (m.size() != (size_t)n * n)
      return false;
    for (auto x : m)
      if (x > 1)
        return false;
  }
  for (const auto &z : c.productByProduction) {
    if (z.size() != (size_t)n * n)
      return false;
    for (auto x : z)
      if (x > (uint64_t)n)
        return false;
  }
  std::vector<Matrix> term(gr.terminals, zeroMatrix(n));
  for (auto e : g.edges)
    at(term[e.label], n, e.from, e.to) = 1;
  for (int pi = 0; pi < (int)gr.productions.size(); ++pi) {
    const auto &p = gr.productions[pi];
    if (p.kind == Production::Epsilon) {
      for (int u = 0; u < n; ++u)
        if (!at(c.M[p.lhs], n, u, u))
          return false;
    } else if (p.kind == Production::Terminal) {
      if (!boolLeq(term[p.terminal], c.M[p.lhs]))
        return false;
    } else {
      const auto &z = c.productByProduction[pi];
      if (z.size() != (size_t)n * n)
        return false;
      Matrix exact = multiply(c.M[p.a], c.M[p.b], n);
      if (exact != z || !boolLeq(z, c.M[p.lhs]))
        return false;
    }
  }
  return at(c.M[gr.start], n, g.source, g.target) == 0;
}
inline void buildNegative(const Grammar &gr, const Graph &g,
                          NegativeCertificate &c) {
  gr.validate();
  if (g.n <= 0 || g.source < 0 || g.source >= g.n || g.target < 0 ||
      g.target >= g.n)
    throw std::invalid_argument("bad graph");
  for (auto e : g.edges)
    if (e.from < 0 || e.from >= g.n || e.to < 0 || e.to >= g.n || e.label < 0 ||
        e.label >= gr.terminals)
      throw std::invalid_argument("bad graph edge");
  int k = gr.nonterminals, n = g.n;
  c.M.assign(k, zeroMatrix(n));
  c.productByProduction.assign(gr.productions.size(), zeroMatrix(n));
  std::vector<Matrix> term(gr.terminals, zeroMatrix(n));
  for (auto e : g.edges)
    at(term[e.label], n, e.from, e.to) = 1;
  for (int pi = 0; pi < (int)gr.productions.size(); ++pi) {
    const auto &p = gr.productions[pi];
    if (p.kind == Production::Epsilon)
      for (int u = 0; u < n; ++u)
        at(c.M[p.lhs], n, u, u) = 1;
    else if (p.kind == Production::Terminal)
      for (size_t z = 0; z < term[p.terminal].size(); ++z)
        if (term[p.terminal][z])
          c.M[p.lhs][z] = 1;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &p : gr.productions)
      if (p.kind == Production::Binary) {
        Matrix z = booleanize(multiply(c.M[p.a], c.M[p.b], n));
        for (size_t i = 0; i < z.size(); ++i)
          if (z[i] && !c.M[p.lhs][i])
            c.M[p.lhs][i] = 1, changed = true;
      }
  }
  for (int pi = 0; pi < (int)gr.productions.size(); ++pi) {
    const auto &p = gr.productions[pi];
    if (p.kind == Production::Binary)
      c.productByProduction[pi] = multiply(c.M[p.a], c.M[p.b], n);
  }
}
} // namespace cflcert::cfg
