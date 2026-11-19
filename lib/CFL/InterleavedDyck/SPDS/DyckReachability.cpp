#include "CFL/InterleavedDyck/SPDS/DyckReachability.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::spds {
namespace {

constexpr std::uint32_t INVALID_ID = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t EMPTY_KEY = std::numeric_limits<std::uint64_t>::max();

std::uint64_t packPairKey(std::uint32_t a, std::uint32_t b) {
  return (static_cast<std::uint64_t>(a) << 32U) | static_cast<std::uint64_t>(b);
}

std::size_t splitmix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
  return static_cast<std::size_t>(x ^ (x >> 31U));
}

class Uint64HashSet {
public:
  bool contains(std::uint64_t key) const {
    if (buckets_.empty())
      return false;
    const std::size_t mask = buckets_.size() - 1;
    std::size_t slot = splitmix64(key) & mask;
    while (buckets_[slot] != EMPTY_KEY) {
      if (buckets_[slot] == key)
        return true;
      slot = (slot + 1) & mask;
    }
    return false;
  }

  bool insert(std::uint64_t key) {
    if (buckets_.empty()) {
      rehash(16);
    } else if ((size_ + 1) * 2 > buckets_.size()) {
      rehash(buckets_.size() * 2);
    }
    const std::size_t mask = buckets_.size() - 1;
    std::size_t slot = splitmix64(key) & mask;
    while (buckets_[slot] != EMPTY_KEY) {
      if (buckets_[slot] == key)
        return false;
      slot = (slot + 1) & mask;
    }
    buckets_[slot] = key;
    ++size_;
    return true;
  }

private:
  void rehash(std::size_t cap) {
    std::vector<std::uint64_t> next(cap, EMPTY_KEY);
    const std::size_t mask = cap - 1;
    for (std::uint64_t key : buckets_) {
      if (key == EMPTY_KEY)
        continue;
      std::size_t slot = splitmix64(key) & mask;
      while (next[slot] != EMPTY_KEY)
        slot = (slot + 1) & mask;
      next[slot] = key;
    }
    buckets_.swap(next);
  }

  std::vector<std::uint64_t> buckets_;
  std::size_t size_ = 0;
};

struct IndexedVertices {
  std::vector<Vertex> verts;

  explicit IndexedVertices(const Graph &graph) : verts(graph.vertices()) {
    std::sort(verts.begin(), verts.end());
  }

  std::uint32_t size() const { return static_cast<std::uint32_t>(verts.size()); }

  std::uint32_t find(Vertex v) const {
    const auto it = std::lower_bound(verts.begin(), verts.end(), v);
    if (it == verts.end() || *it != v)
      return INVALID_ID;
    return static_cast<std::uint32_t>(it - verts.begin());
  }
};

struct SccResult {
  std::uint32_t num_sccs = 0;
  std::vector<std::uint32_t> scc_of;
};

SccResult computeIterativeTarjanScc(
    std::uint32_t n, const std::vector<std::vector<std::uint32_t>> &adj) {
  SccResult res;
  res.scc_of.assign(n, INVALID_ID);
  if (n == 0)
    return res;

  std::vector<std::uint32_t> index(n, INVALID_ID);
  std::vector<std::uint32_t> low(n, 0);
  std::vector<std::uint8_t> on_stack(n, 0);
  std::vector<std::uint32_t> tarjan_stack;
  tarjan_stack.reserve(n);

  struct Frame {
    std::uint32_t u;
    std::size_t cursor;
  };
  std::vector<Frame> dfs;
  dfs.reserve(std::min<std::size_t>(n, 1024));

  std::uint32_t next_idx = 0;
  for (std::uint32_t root = 0; root < n; ++root) {
    if (index[root] != INVALID_ID)
      continue;
    index[root] = low[root] = next_idx++;
    on_stack[root] = 1;
    tarjan_stack.push_back(root);
    dfs.push_back({root, 0});

    while (!dfs.empty()) {
      Frame &top = dfs.back();
      const std::uint32_t u = top.u;
      const auto &nbrs = adj[u];
      if (top.cursor < nbrs.size()) {
        const std::uint32_t w = nbrs[top.cursor++];
        if (index[w] == INVALID_ID) {
          index[w] = low[w] = next_idx++;
          on_stack[w] = 1;
          tarjan_stack.push_back(w);
          dfs.push_back({w, 0});
        } else if (on_stack[w]) {
          low[u] = std::min(low[u], index[w]);
        }
      } else {
        dfs.pop_back();
        if (!dfs.empty()) {
          const std::uint32_t p = dfs.back().u;
          low[p] = std::min(low[p], low[u]);
        }
        if (low[u] == index[u]) {
          const std::uint32_t cid = res.num_sccs++;
          while (true) {
            const std::uint32_t x = tarjan_stack.back();
            tarjan_stack.pop_back();
            on_stack[x] = 0;
            res.scc_of[x] = cid;
            if (x == u)
              break;
          }
        }
      }
    }
  }
  return res;
}

struct SccDag {
  std::uint32_t num_sccs = 0;
  std::vector<std::uint32_t> scc_of;
  std::vector<std::vector<std::uint32_t>> dag_fwd;
  std::vector<std::uint32_t> topo;
};

SccDag buildSccDag(std::uint32_t n,
                   const std::vector<std::vector<std::uint32_t>> &adj) {
  SccDag dag;
  auto scc = computeIterativeTarjanScc(n, adj);
  dag.num_sccs = scc.num_sccs;
  dag.scc_of = std::move(scc.scc_of);
  dag.dag_fwd.resize(dag.num_sccs);
  for (std::uint32_t u = 0; u < n; ++u) {
    const std::uint32_t cu = dag.scc_of[u];
    for (std::uint32_t v : adj[u]) {
      const std::uint32_t cv = dag.scc_of[v];
      if (cu != cv)
        dag.dag_fwd[cu].push_back(cv);
    }
  }
  std::vector<std::uint32_t> in_deg(dag.num_sccs, 0);
  for (std::uint32_t c = 0; c < dag.num_sccs; ++c) {
    auto &nbrs = dag.dag_fwd[c];
    std::sort(nbrs.begin(), nbrs.end());
    nbrs.erase(std::unique(nbrs.begin(), nbrs.end()), nbrs.end());
    for (std::uint32_t nxt : nbrs)
      ++in_deg[nxt];
  }
  dag.topo.reserve(dag.num_sccs);
  for (std::uint32_t c = 0; c < dag.num_sccs; ++c)
    if (in_deg[c] == 0)
      dag.topo.push_back(c);
  for (std::size_t head = 0; head < dag.topo.size(); ++head) {
    const std::uint32_t u = dag.topo[head];
    for (std::uint32_t v : dag.dag_fwd[u]) {
      if (--in_deg[v] == 0)
        dag.topo.push_back(v);
    }
  }
  return dag;
}

std::vector<std::uint8_t>
denseBfs(std::uint32_t n, const std::vector<std::vector<std::uint32_t>> &adj,
         const std::vector<std::uint32_t> &seeds) {
  std::vector<std::uint8_t> seen(n, 0);
  std::vector<std::uint32_t> q;
  q.reserve(seeds.size());
  for (std::uint32_t s : seeds) {
    if (s < n && !seen[s]) {
      seen[s] = 1;
      q.push_back(s);
    }
  }
  for (std::size_t head = 0; head < q.size(); ++head) {
    for (std::uint32_t nxt : adj[q[head]]) {
      if (!seen[nxt]) {
        seen[nxt] = 1;
        q.push_back(nxt);
      }
    }
  }
  return seen;
}

Graph sliceGraphImpl(const Graph &graph, const std::vector<Pair> &pairs,
                     bool ignore_self_pairs,
                     const std::vector<Vertex> *extra_sources,
                     const std::vector<Vertex> *extra_targets) {
  const auto &edges = graph.edges();
  if (edges.empty())
    return {};

  const IndexedVertices ig(graph);
  const std::uint32_t n = ig.size();
  std::vector<std::pair<std::uint32_t, std::uint32_t>> edge_ends(edges.size());
  std::vector<std::vector<std::uint32_t>> fwd(n), rev(n);
  for (std::size_t i = 0; i < edges.size(); ++i) {
    const std::uint32_t u = ig.find(edges[i].source);
    const std::uint32_t v = ig.find(edges[i].target);
    edge_ends[i] = {u, v};
    fwd[u].push_back(v);
    rev[v].push_back(u);
  }

  const SccDag dag = buildSccDag(n, fwd);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> valid_pairs;
  valid_pairs.reserve(pairs.size());
  for (const Pair &p : pairs) {
    const std::uint32_t u = ig.find(p.source);
    const std::uint32_t v = ig.find(p.target);
    if (u == INVALID_ID || v == INVALID_ID)
      continue;
    if (ignore_self_pairs && u == v)
      continue;
    valid_pairs.emplace_back(u, v);
  }

  std::vector<std::uint8_t> keep(edges.size(), 0);
  if (!valid_pairs.empty()) {
    std::vector<std::uint32_t> uniq_srcs, uniq_dsts;
    uniq_srcs.reserve(valid_pairs.size());
    uniq_dsts.reserve(valid_pairs.size());
    for (const auto &[u, v] : valid_pairs) {
      uniq_srcs.push_back(u);
      uniq_dsts.push_back(v);
    }
    std::sort(uniq_srcs.begin(), uniq_srcs.end());
    uniq_srcs.erase(std::unique(uniq_srcs.begin(), uniq_srcs.end()),
                    uniq_srcs.end());
    std::sort(uniq_dsts.begin(), uniq_dsts.end());
    uniq_dsts.erase(std::unique(uniq_dsts.begin(), uniq_dsts.end()),
                    uniq_dsts.end());

    const bool by_source = uniq_srcs.size() <= uniq_dsts.size();
    std::vector<std::pair<std::uint32_t, std::uint32_t>> grouped;
    grouped.reserve(valid_pairs.size());
    for (const auto &[u, v] : valid_pairs)
      grouped.emplace_back(by_source ? u : v, by_source ? v : u);
    std::sort(grouped.begin(), grouped.end());
    grouped.erase(std::unique(grouped.begin(), grouped.end()), grouped.end());

    std::vector<std::uint32_t> anchor_starts;
    for (std::size_t i = 0; i < grouped.size();) {
      anchor_starts.push_back(static_cast<std::uint32_t>(i));
      const std::uint32_t anchor = grouped[i].first;
      while (i < grouped.size() && grouped[i].first == anchor)
        ++i;
    }
    anchor_starts.push_back(static_cast<std::uint32_t>(grouped.size()));

    const std::size_t num_anchors = anchor_starts.size() - 1;
    std::vector<std::uint64_t> fwd_mask(dag.num_sccs, 0ULL);
    std::vector<std::uint64_t> rev_mask(dag.num_sccs, 0ULL);
    for (std::size_t base = 0; base < num_anchors; base += 64) {
      const std::size_t batch = std::min<std::size_t>(64, num_anchors - base);
      std::fill(fwd_mask.begin(), fwd_mask.end(), 0ULL);
      std::fill(rev_mask.begin(), rev_mask.end(), 0ULL);
      for (std::size_t bit_idx = 0; bit_idx < batch; ++bit_idx) {
        const std::uint64_t bit = 1ULL << bit_idx;
        const std::size_t lo = anchor_starts[base + bit_idx];
        const std::size_t hi = anchor_starts[base + bit_idx + 1];
        const std::uint32_t anchor_scc = dag.scc_of[grouped[lo].first];
        if (by_source) {
          fwd_mask[anchor_scc] |= bit;
          for (std::size_t p = lo; p < hi; ++p)
            rev_mask[dag.scc_of[grouped[p].second]] |= bit;
        } else {
          rev_mask[anchor_scc] |= bit;
          for (std::size_t p = lo; p < hi; ++p)
            fwd_mask[dag.scc_of[grouped[p].second]] |= bit;
        }
      }
      for (std::uint32_t cu : dag.topo) {
        const std::uint64_t m = fwd_mask[cu];
        if (!m)
          continue;
        for (std::uint32_t cv : dag.dag_fwd[cu])
          fwd_mask[cv] |= m;
      }
      for (auto it = dag.topo.rbegin(); it != dag.topo.rend(); ++it) {
        const std::uint32_t cu = *it;
        std::uint64_t m = rev_mask[cu];
        for (std::uint32_t cv : dag.dag_fwd[cu])
          m |= rev_mask[cv];
        rev_mask[cu] = m;
      }
      for (std::size_t i = 0; i < edges.size(); ++i) {
        if (!keep[i] &&
            (fwd_mask[dag.scc_of[edge_ends[i].first]] &
             rev_mask[dag.scc_of[edge_ends[i].second]]) != 0ULL) {
          keep[i] = 1;
        }
      }
    }
  }

  if (extra_sources && !extra_sources->empty()) {
    std::vector<std::uint32_t> seeds;
    seeds.reserve(extra_sources->size());
    for (Vertex s : *extra_sources) {
      const std::uint32_t idx = ig.find(s);
      if (idx != INVALID_ID)
        seeds.push_back(idx);
    }
    if (!seeds.empty()) {
      const auto from_src = denseBfs(n, fwd, seeds);
      for (std::size_t i = 0; i < edges.size(); ++i) {
        if (from_src[edge_ends[i].first])
          keep[i] = 1;
      }
    }
  }

  if (extra_targets && !extra_targets->empty()) {
    std::vector<std::uint32_t> seeds;
    seeds.reserve(extra_targets->size());
    for (Vertex t : *extra_targets) {
      const std::uint32_t idx = ig.find(t);
      if (idx != INVALID_ID)
        seeds.push_back(idx);
    }
    if (!seeds.empty()) {
      const auto to_dst = denseBfs(n, rev, seeds);
      for (std::size_t i = 0; i < edges.size(); ++i) {
        if (to_dst[edge_ends[i].second])
          keep[i] = 1;
      }
    }
  }

  Graph result;
  for (std::size_t i = 0; i < edges.size(); ++i) {
    if (keep[i]) {
      const Edge &e = edges[i];
      result.addEdge(e.source, e.target, e.label);
    }
  }
  return result;
}

enum EdgeTag : std::uint8_t { TAG_EPS = 0, TAG_OPEN = 1, TAG_CLOSE = 2 };

struct CondensedEdge {
  std::uint32_t u = 0;
  std::uint32_t v = 0;
  std::uint8_t kind = TAG_EPS;
  std::uint64_t sym = 0;

  bool operator<(const CondensedEdge &o) const {
    return std::tie(u, v, kind, sym) < std::tie(o.u, o.v, o.kind, o.sym);
  }
  bool operator==(const CondensedEdge &o) const {
    return u == o.u && v == o.v && kind == o.kind && sym == o.sym;
  }
};

struct CondensedDyckSystem {
  std::uint32_t cur_v = 0;
  std::vector<std::uint32_t> rep; // original vertex index -> condensed vertex
  std::vector<CondensedEdge> edges;
};

std::pair<std::uint8_t, std::uint64_t> encodeEdgeLabel(const Label &lbl,
                                                       DyckProjectionKind kind) {
  switch (kind) {
  case DyckProjectionKind::Parentheses:
    if (lbl.kind == LabelKind::OpenParenthesis)
      return {TAG_OPEN, lbl.id};
    if (lbl.kind == LabelKind::CloseParenthesis)
      return {TAG_CLOSE, lbl.id};
    return {TAG_EPS, 0};
  case DyckProjectionKind::Brackets:
    if (lbl.kind == LabelKind::OpenBracket)
      return {TAG_OPEN, lbl.id};
    if (lbl.kind == LabelKind::CloseBracket)
      return {TAG_CLOSE, lbl.id};
    return {TAG_EPS, 0};
  case DyckProjectionKind::UnionDyck:
    if (lbl.kind == LabelKind::OpenParenthesis)
      return {TAG_OPEN, static_cast<std::uint64_t>(lbl.id) << 1U};
    if (lbl.kind == LabelKind::CloseParenthesis)
      return {TAG_CLOSE, static_cast<std::uint64_t>(lbl.id) << 1U};
    if (lbl.kind == LabelKind::OpenBracket)
      return {TAG_OPEN, (static_cast<std::uint64_t>(lbl.id) << 1U) | 1ULL};
    if (lbl.kind == LabelKind::CloseBracket)
      return {TAG_CLOSE, (static_cast<std::uint64_t>(lbl.id) << 1U) | 1ULL};
    return {TAG_EPS, 0};
  }
  return {TAG_EPS, 0};
}

CondensedDyckSystem condenseDyckGraph(const Graph &graph,
                                      const IndexedVertices &ig,
                                      DyckProjectionKind kind) {
  CondensedDyckSystem sys;
  const std::uint32_t n = ig.size();
  sys.cur_v = n;
  sys.rep.resize(n);
  for (std::uint32_t i = 0; i < n; ++i)
    sys.rep[i] = i;

  sys.edges.reserve(graph.edges().size());
  for (const Edge &e : graph.edges()) {
    const auto [ekind, esym] = encodeEdgeLabel(e.label, kind);
    sys.edges.push_back({ig.find(e.source), ig.find(e.target), ekind, esym});
  }

  // Hub vertices (mid, sym) -> hub_vid keep midpoint-Dyck shortcuts linear in
  // |opens| + |closes| instead of quadratic |opens| * |closes|.
  std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint32_t> bridged_hubs;

  struct MidEndpoint {
    std::uint32_t mid;
    std::uint64_t sym;
    std::uint32_t endpoint;
    bool operator<(const MidEndpoint &o) const {
      return std::tie(mid, sym, endpoint) < std::tie(o.mid, o.sym, o.endpoint);
    }
    bool operator==(const MidEndpoint &o) const {
      return mid == o.mid && sym == o.sym && endpoint == o.endpoint;
    }
  };

  while (true) {
    std::vector<std::vector<std::uint32_t>> eps_adj(sys.cur_v);
    for (const auto &e : sys.edges) {
      if (e.kind == TAG_EPS && e.u != e.v)
        eps_adj[e.u].push_back(e.v);
    }
    const auto scc = computeIterativeTarjanScc(sys.cur_v, eps_adj);
    for (std::uint32_t i = 0; i < n; ++i)
      sys.rep[i] = scc.scc_of[sys.rep[i]];

    if (!bridged_hubs.empty()) {
      std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint32_t> next_hubs;
      for (const auto &[ms, hid] : bridged_hubs) {
        next_hubs.emplace(std::make_pair(scc.scc_of[ms.first], ms.second),
                          scc.scc_of[hid]);
      }
      bridged_hubs.swap(next_hubs);
    }

    for (auto &e : sys.edges) {
      e.u = scc.scc_of[e.u];
      e.v = scc.scc_of[e.v];
    }
    sys.edges.erase(
        std::remove_if(sys.edges.begin(), sys.edges.end(),
                       [](const CondensedEdge &e) {
                         return e.kind == TAG_EPS && e.u == e.v;
                       }),
        sys.edges.end());
    std::sort(sys.edges.begin(), sys.edges.end());
    sys.edges.erase(std::unique(sys.edges.begin(), sys.edges.end()),
                    sys.edges.end());

    std::unordered_set<std::uint64_t> has_open, has_close;
    for (const auto &e : sys.edges) {
      if (e.kind == TAG_OPEN)
        has_open.insert(e.sym);
      else if (e.kind == TAG_CLOSE)
        has_close.insert(e.sym);
    }
    std::unordered_set<std::uint64_t> matched;
    for (std::uint64_t sym : has_open) {
      if (has_close.count(sym))
        matched.insert(sym);
    }

    std::vector<CondensedEdge> next_edges;
    next_edges.reserve(sys.edges.size());
    Uint64HashSet seen_eps;
    std::vector<MidEndpoint> open_list, close_list;
    for (const auto &e : sys.edges) {
      if (e.kind != TAG_EPS && matched.count(e.sym) == 0U)
        continue;
      next_edges.push_back(e);
      if (e.kind == TAG_EPS) {
        seen_eps.insert(packPairKey(e.u, e.v));
      } else if (e.kind == TAG_OPEN) {
        open_list.push_back({e.v, e.sym, e.u});
      } else {
        close_list.push_back({e.u, e.sym, e.v});
      }
    }

    std::sort(open_list.begin(), open_list.end());
    open_list.erase(std::unique(open_list.begin(), open_list.end()),
                    open_list.end());
    std::sort(close_list.begin(), close_list.end());
    close_list.erase(std::unique(close_list.begin(), close_list.end()),
                     close_list.end());

    std::uint32_t next_vid = scc.num_sccs;
    std::size_t added_eps = 0;
    std::size_t oi = 0, ci = 0;
    while (oi < open_list.size() && ci < close_list.size()) {
      const auto o_key = std::tie(open_list[oi].mid, open_list[oi].sym);
      const auto c_key = std::tie(close_list[ci].mid, close_list[ci].sym);
      if (o_key < c_key) {
        while (oi < open_list.size() &&
               std::tie(open_list[oi].mid, open_list[oi].sym) == o_key)
          ++oi;
        continue;
      }
      if (c_key < o_key) {
        while (ci < close_list.size() &&
               std::tie(close_list[ci].mid, close_list[ci].sym) == c_key)
          ++ci;
        continue;
      }

      const std::uint32_t mid = open_list[oi].mid;
      const std::uint64_t sym = open_list[oi].sym;
      const std::size_t o_end = [&] {
        std::size_t k = oi;
        while (k < open_list.size() && open_list[k].mid == mid &&
               open_list[k].sym == sym)
          ++k;
        return k;
      }();
      const std::size_t c_end = [&] {
        std::size_t k = ci;
        while (k < close_list.size() && close_list[k].mid == mid &&
               close_list[k].sym == sym)
          ++k;
        return k;
      }();

      const std::size_t o_cnt = o_end - oi;
      const std::size_t c_cnt = c_end - ci;
      const auto hub_it = bridged_hubs.find({mid, sym});
      if (hub_it == bridged_hubs.end() && o_cnt * c_cnt <= 8U) {
        for (std::size_t a = oi; a < o_end; ++a) {
          const std::uint32_t u = open_list[a].endpoint;
          for (std::size_t b = ci; b < c_end; ++b) {
            const std::uint32_t w = close_list[b].endpoint;
            if (u != w && seen_eps.insert(packPairKey(u, w))) {
              next_edges.push_back({u, w, TAG_EPS, 0});
              ++added_eps;
            }
          }
        }
      } else {
        std::uint32_t hid = 0;
        if (hub_it != bridged_hubs.end()) {
          hid = hub_it->second;
        } else {
          hid = next_vid++;
          bridged_hubs.emplace(std::make_pair(mid, sym), hid);
        }
        for (std::size_t a = oi; a < o_end; ++a) {
          const std::uint32_t u = open_list[a].endpoint;
          if (u != hid && seen_eps.insert(packPairKey(u, hid))) {
            next_edges.push_back({u, hid, TAG_EPS, 0});
            ++added_eps;
          }
        }
        for (std::size_t b = ci; b < c_end; ++b) {
          const std::uint32_t w = close_list[b].endpoint;
          if (hid != w && seen_eps.insert(packPairKey(hid, w))) {
            next_edges.push_back({hid, w, TAG_EPS, 0});
            ++added_eps;
          }
        }
      }

      oi = o_end;
      ci = c_end;
    }

    sys.cur_v = next_vid;
    sys.edges.swap(next_edges);
    if (added_eps == 0U)
      break;
  }

  return sys;
}

/// Runs direction-adaptive shared RHS summary tabulation on `0 .. L-1` from
/// `seeds`, then evaluates reachability over `(eps U summary)` via 64-way
/// bit-parallel SCC-DAG sweeps. `queries` holds `(seed_live, partner_live)`
/// and `on_match(query_index)` is invoked for every reachable query.
template <class OnMatch>
void runSharedRhsTabulation(
    std::uint32_t L, const std::vector<CondensedEdge> &live_edges,
    bool reverse_direction, const std::vector<std::uint32_t> &seeds,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> &queries,
    OnMatch &&on_match) {
  if (L == 0 || seeds.empty() || queries.empty())
    return;

  std::vector<std::vector<std::uint32_t>> eps_out(L);
  std::vector<std::vector<std::pair<std::uint64_t, std::uint32_t>>> op_out(L),
      cp_out(L);

  struct SlotKey {
    std::uint32_t ent;
    std::uint64_t sym;
    bool operator<(const SlotKey &o) const {
      return std::tie(ent, sym) < std::tie(o.ent, o.sym);
    }
    bool operator==(const SlotKey &o) const {
      return ent == o.ent && sym == o.sym;
    }
  };
  std::vector<SlotKey> slot_keys;

  for (const auto &e : live_edges) {
    const std::uint32_t from = reverse_direction ? e.v : e.u;
    const std::uint32_t to = reverse_direction ? e.u : e.v;
    std::uint8_t k = e.kind;
    if (reverse_direction) {
      if (k == TAG_OPEN)
        k = TAG_CLOSE;
      else if (k == TAG_CLOSE)
        k = TAG_OPEN;
    }
    if (k == TAG_EPS) {
      eps_out[from].push_back(to);
    } else if (k == TAG_OPEN) {
      op_out[from].emplace_back(e.sym, to);
      slot_keys.push_back({to, e.sym});
    } else {
      cp_out[from].emplace_back(e.sym, to);
    }
  }

  std::sort(slot_keys.begin(), slot_keys.end());
  slot_keys.erase(std::unique(slot_keys.begin(), slot_keys.end()),
                  slot_keys.end());

  const auto find_slot = [&](std::uint32_t ent,
                             std::uint64_t sym) -> std::uint32_t {
    const SlotKey target{ent, sym};
    const auto it = std::lower_bound(slot_keys.begin(), slot_keys.end(), target);
    if (it == slot_keys.end() || !(*it == target))
      return INVALID_ID;
    return static_cast<std::uint32_t>(it - slot_keys.begin());
  };

  const std::uint32_t num_slots = static_cast<std::uint32_t>(slot_keys.size());
  std::vector<std::vector<std::uint32_t>> callers(num_slots), rets(num_slots);
  Uint64HashSet seen_slot_caller, seen_slot_ret;

  std::vector<std::uint8_t> vis_any(L, 0);
  std::deque<std::uint32_t> q_any;
  std::vector<std::uint32_t> ent_to_id(L, INVALID_ID);
  std::vector<std::uint32_t> id_to_ent;
  Uint64HashSet seen_ent_pe, seen_sum;
  std::vector<std::vector<std::uint32_t>> rev_ent_pe(L);
  std::vector<std::vector<std::uint32_t>> sum_adj(L);
  std::deque<std::pair<std::uint32_t, std::uint32_t>> q_ent;

  auto add_ent = [&](std::uint32_t e_id, std::uint32_t x) {
    if (seen_ent_pe.insert(packPairKey(e_id, x))) {
      rev_ent_pe[x].push_back(e_id);
      q_ent.emplace_back(e_id, x);
    }
  };

  auto add_sum = [&](std::uint32_t cu, std::uint32_t r) {
    if (cu == r)
      return;
    if (!seen_sum.insert(packPairKey(cu, r)))
      return;
    sum_adj[cu].push_back(r);
    if (vis_any[cu] && !vis_any[r]) {
      vis_any[r] = 1;
      q_any.push_back(r);
    }
    const std::size_t rev_n = rev_ent_pe[cu].size();
    for (std::size_t i = 0; i < rev_n; ++i)
      add_ent(rev_ent_pe[cu][i], r);
  };

  for (std::uint32_t s : seeds) {
    if (!vis_any[s]) {
      vis_any[s] = 1;
      q_any.push_back(s);
    }
  }

  while (!q_any.empty() || !q_ent.empty()) {
    while (!q_any.empty()) {
      const std::uint32_t x = q_any.front();
      q_any.pop_front();
      for (std::uint32_t y : eps_out[x]) {
        if (!vis_any[y]) {
          vis_any[y] = 1;
          q_any.push_back(y);
        }
      }
      const std::size_t sn = sum_adj[x].size();
      for (std::size_t si = 0; si < sn; ++si) {
        const std::uint32_t y = sum_adj[x][si];
        if (!vis_any[y]) {
          vis_any[y] = 1;
          q_any.push_back(y);
        }
      }
      for (const auto &[sym, ent] : op_out[x]) {
        if (!vis_any[ent]) {
          vis_any[ent] = 1;
          q_any.push_back(ent);
        }
        const std::uint32_t slot = find_slot(ent, sym);
        if (seen_slot_caller.insert(packPairKey(slot, x))) {
          callers[slot].push_back(x);
          const std::size_t rn = rets[slot].size();
          for (std::size_t ri = 0; ri < rn; ++ri)
            add_sum(x, rets[slot][ri]);
        }
        std::uint32_t e_id = ent_to_id[ent];
        if (e_id == INVALID_ID) {
          e_id = static_cast<std::uint32_t>(id_to_ent.size());
          ent_to_id[ent] = e_id;
          id_to_ent.push_back(ent);
          add_ent(e_id, ent);
        }
      }
    }
    if (!q_ent.empty()) {
      const auto [e_id, x] = q_ent.front();
      q_ent.pop_front();
      const std::uint32_t ent = id_to_ent[e_id];
      for (std::uint32_t y : eps_out[x])
        add_ent(e_id, y);
      const std::size_t sn = sum_adj[x].size();
      for (std::size_t si = 0; si < sn; ++si)
        add_ent(e_id, sum_adj[x][si]);
      for (const auto &[sym, r] : cp_out[x]) {
        const std::uint32_t slot = find_slot(ent, sym);
        if (slot == INVALID_ID)
          continue;
        if (seen_slot_ret.insert(packPairKey(slot, r))) {
          rets[slot].push_back(r);
          const std::size_t cn = callers[slot].size();
          for (std::size_t ci = 0; ci < cn; ++ci)
            add_sum(callers[slot][ci], r);
        }
      }
    }
  }

  // Same-level reachability over (eps_out U sum_adj) via 64-way bit-parallel
  // DAG propagation.
  std::vector<std::vector<std::uint32_t>> same_adj(L);
  for (std::uint32_t u = 0; u < L; ++u) {
    same_adj[u].reserve(eps_out[u].size() + sum_adj[u].size());
    same_adj[u].insert(same_adj[u].end(), eps_out[u].begin(), eps_out[u].end());
    same_adj[u].insert(same_adj[u].end(), sum_adj[u].begin(), sum_adj[u].end());
  }
  const SccDag same_dag = buildSccDag(L, same_adj);

  std::vector<std::uint32_t> q_indices(queries.size());
  for (std::size_t i = 0; i < queries.size(); ++i)
    q_indices[i] = static_cast<std::uint32_t>(i);
  std::sort(q_indices.begin(), q_indices.end(),
            [&](std::uint32_t a, std::uint32_t b) {
              return queries[a].first < queries[b].first;
            });

  std::vector<std::pair<std::size_t, std::size_t>> seed_spans;
  for (std::size_t i = 0; i < q_indices.size();) {
    const std::uint32_t seed_v = queries[q_indices[i]].first;
    std::size_t j = i + 1;
    while (j < q_indices.size() && queries[q_indices[j]].first == seed_v)
      ++j;
    seed_spans.emplace_back(i, j);
    i = j;
  }

  std::vector<std::uint64_t> reach_mask(same_dag.num_sccs, 0ULL);
  for (std::size_t base = 0; base < seed_spans.size(); base += 64) {
    const std::size_t batch =
        std::min<std::size_t>(64, seed_spans.size() - base);
    std::fill(reach_mask.begin(), reach_mask.end(), 0ULL);
    for (std::size_t bit_idx = 0; bit_idx < batch; ++bit_idx) {
      const std::uint32_t seed_v =
          queries[q_indices[seed_spans[base + bit_idx].first]].first;
      reach_mask[same_dag.scc_of[seed_v]] |= (1ULL << bit_idx);
    }
    for (std::uint32_t cu : same_dag.topo) {
      const std::uint64_t m = reach_mask[cu];
      if (!m)
        continue;
      for (std::uint32_t cv : same_dag.dag_fwd[cu])
        reach_mask[cv] |= m;
    }
    for (std::size_t bit_idx = 0; bit_idx < batch; ++bit_idx) {
      const std::uint64_t bit = 1ULL << bit_idx;
      const auto [lo, hi] = seed_spans[base + bit_idx];
      for (std::size_t pos = lo; pos < hi; ++pos) {
        const std::uint32_t qi = q_indices[pos];
        const std::uint32_t partner = queries[qi].second;
        if ((reach_mask[same_dag.scc_of[partner]] & bit) != 0ULL)
          on_match(qi);
      }
    }
  }
}

std::pair<std::size_t, std::size_t> countParenAndBracketEdges(const Graph &g) {
  std::size_t par = 0, brk = 0;
  for (const Edge &e : g.edges()) {
    if (e.label.kind == LabelKind::OpenParenthesis ||
        e.label.kind == LabelKind::CloseParenthesis)
      ++par;
    else if (e.label.kind == LabelKind::OpenBracket ||
             e.label.kind == LabelKind::CloseBracket)
      ++brk;
  }
  return {par, brk};
}

} // namespace

PairSet filterDyckReachableDemands(const Graph &graph,
                                   const std::vector<Pair> &demands,
                                   DyckProjectionKind kind,
                                   bool include_self_pairs) {
  PairSet accepted;
  if (graph.vertices().empty() || demands.empty())
    return accepted;

  const IndexedVertices ig(graph);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> indexed_demands;
  indexed_demands.reserve(demands.size());
  for (const Pair &p : demands) {
    const std::uint32_t u = ig.find(p.source);
    const std::uint32_t v = ig.find(p.target);
    if (u == INVALID_ID || v == INVALID_ID)
      continue;
    if (u == v) {
      if (include_self_pairs)
        accepted.insert(p);
      continue;
    }
    indexed_demands.emplace_back(u, v);
  }
  if (indexed_demands.empty())
    return accepted;

  std::sort(indexed_demands.begin(), indexed_demands.end());
  indexed_demands.erase(
      std::unique(indexed_demands.begin(), indexed_demands.end()),
      indexed_demands.end());

  const auto sys = condenseDyckGraph(graph, ig, kind);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> rem_orig;
  rem_orig.reserve(indexed_demands.size());
  for (const auto &[u, v] : indexed_demands) {
    if (sys.rep[u] == sys.rep[v]) {
      accepted.insert({ig.verts[u], ig.verts[v]});
    } else {
      rem_orig.emplace_back(u, v);
    }
  }
  if (rem_orig.empty() || sys.edges.empty())
    return accepted;

  std::vector<std::vector<std::uint32_t>> fwd_all(sys.cur_v),
      rev_all(sys.cur_v);
  for (const auto &e : sys.edges) {
    fwd_all[e.u].push_back(e.v);
    rev_all[e.v].push_back(e.u);
  }
  std::vector<std::uint32_t> cond_srcs, cond_dsts;
  cond_srcs.reserve(rem_orig.size());
  cond_dsts.reserve(rem_orig.size());
  for (const auto &[u, v] : rem_orig) {
    cond_srcs.push_back(sys.rep[u]);
    cond_dsts.push_back(sys.rep[v]);
  }
  const auto fwd_live = denseBfs(sys.cur_v, fwd_all, cond_srcs);
  const auto rev_live = denseBfs(sys.cur_v, rev_all, cond_dsts);

  std::vector<std::uint32_t> live_id(sys.cur_v, INVALID_ID);
  std::uint32_t L = 0;
  for (std::uint32_t v = 0; v < sys.cur_v; ++v) {
    if (fwd_live[v] && rev_live[v])
      live_id[v] = L++;
  }
  if (L == 0)
    return accepted;

  std::vector<std::pair<std::uint32_t, std::uint32_t>> live_rem;
  live_rem.reserve(rem_orig.size());
  for (const auto &[u, v] : rem_orig) {
    if (live_id[sys.rep[u]] != INVALID_ID && live_id[sys.rep[v]] != INVALID_ID)
      live_rem.emplace_back(u, v);
  }
  if (live_rem.empty())
    return accepted;

  std::vector<CondensedEdge> live_edges;
  live_edges.reserve(sys.edges.size());
  std::unordered_set<std::uint64_t> has_o, has_c;
  for (const auto &e : sys.edges) {
    const std::uint32_t lu = live_id[e.u];
    const std::uint32_t lv = live_id[e.v];
    if (lu == INVALID_ID || lv == INVALID_ID)
      continue;
    live_edges.push_back({lu, lv, e.kind, e.sym});
    if (e.kind == TAG_OPEN)
      has_o.insert(e.sym);
    else if (e.kind == TAG_CLOSE)
      has_c.insert(e.sym);
  }
  live_edges.erase(
      std::remove_if(live_edges.begin(), live_edges.end(),
                     [&](const CondensedEdge &e) {
                       return e.kind != TAG_EPS &&
                              (has_o.count(e.sym) == 0U ||
                               has_c.count(e.sym) == 0U);
                     }),
      live_edges.end());

  std::vector<std::uint32_t> u_srcs, u_dsts;
  u_srcs.reserve(live_rem.size());
  u_dsts.reserve(live_rem.size());
  for (const auto &[u, v] : live_rem) {
    u_srcs.push_back(live_id[sys.rep[u]]);
    u_dsts.push_back(live_id[sys.rep[v]]);
  }
  std::sort(u_srcs.begin(), u_srcs.end());
  u_srcs.erase(std::unique(u_srcs.begin(), u_srcs.end()), u_srcs.end());
  std::sort(u_dsts.begin(), u_dsts.end());
  u_dsts.erase(std::unique(u_dsts.begin(), u_dsts.end()), u_dsts.end());

  const bool rev_dir = u_dsts.size() < u_srcs.size();
  const auto &seeds = rev_dir ? u_dsts : u_srcs;

  std::vector<std::pair<std::uint32_t, std::uint32_t>> tab_queries;
  tab_queries.reserve(live_rem.size());
  for (const auto &[u, v] : live_rem) {
    const std::uint32_t lu = live_id[sys.rep[u]];
    const std::uint32_t lv = live_id[sys.rep[v]];
    tab_queries.emplace_back(rev_dir ? lv : lu, rev_dir ? lu : lv);
  }

  runSharedRhsTabulation(L, live_edges, rev_dir, seeds, tab_queries,
                         [&](std::size_t qi) {
                           const auto [u, v] = live_rem[qi];
                           accepted.insert({ig.verts[u], ig.verts[v]});
                         });
  return accepted;
}

std::vector<Vertex> queryDyckReachableFromAnchor(const Graph &graph,
                                                 Vertex anchor,
                                                 Direction direction,
                                                 DyckProjectionKind kind) {
  if (!graph.containsVertex(anchor))
    return {};

  const IndexedVertices ig(graph);
  const std::uint32_t anchor_idx = ig.find(anchor);
  if (anchor_idx == INVALID_ID)
    return {};

  const std::uint32_t n = ig.size();
  std::vector<std::vector<std::uint32_t>> adj(n);
  for (const Edge &e : graph.edges()) {
    const std::uint32_t u = ig.find(e.source);
    const std::uint32_t v = ig.find(e.target);
    if (direction == Direction::Post)
      adj[u].push_back(v);
    else
      adj[v].push_back(u);
  }
  const auto ord_reach = denseBfs(n, adj, {anchor_idx});

  std::vector<Pair> demands;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (!ord_reach[i])
      continue;
    if (direction == Direction::Post)
      demands.push_back({anchor, ig.verts[i]});
    else
      demands.push_back({ig.verts[i], anchor});
  }

  const PairSet kept = filterDyckReachableDemands(graph, demands, kind, true);
  std::vector<Vertex> out;
  out.reserve(kept.size());
  for (const Pair &p : kept)
    out.push_back(direction == Direction::Post ? p.target : p.source);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

PairSet filterInterleavedBooleanDemands(const Graph &graph,
                                        const std::vector<Pair> &demands,
                                        bool include_self_pairs) {
  PairSet result;
  if (graph.vertices().empty() || demands.empty())
    return result;

  std::vector<Pair> non_self;
  non_self.reserve(demands.size());
  for (const Pair &p : demands) {
    if (!graph.containsVertex(p.source) || !graph.containsVertex(p.target))
      continue;
    if (p.source == p.target) {
      if (include_self_pairs)
        result.insert(p);
    } else {
      non_self.push_back(p);
    }
  }
  if (non_self.empty())
    return result;

  const auto [par_cnt, brk_cnt] = countParenAndBracketEdges(graph);
  const DyckProjectionKind first_kind = brk_cnt <= par_cnt
                                            ? DyckProjectionKind::Brackets
                                            : DyckProjectionKind::Parentheses;
  const DyckProjectionKind second_kind = brk_cnt <= par_cnt
                                             ? DyckProjectionKind::Parentheses
                                             : DyckProjectionKind::Brackets;

  const PairSet first_kept =
      filterDyckReachableDemands(graph, non_self, first_kind, false);
  if (first_kept.empty())
    return result;

  const Graph sliced = sliceGraphOnCandidatePaths(graph, first_kept);
  const std::vector<Pair> survivors(first_kept.begin(), first_kept.end());
  PairSet second_kept =
      filterDyckReachableDemands(sliced, survivors, second_kind, false);
  for (const Pair &p : result)
    second_kept.insert(p);
  return second_kept;
}

PairSet filterInterleavedBooleanAll(const Graph &graph,
                                    bool include_self_pairs) {
  PairSet result;
  if (graph.vertices().empty())
    return result;

  if (include_self_pairs) {
    for (Vertex v : graph.vertices())
      result.insert({v, v});
  }
  if (graph.edges().empty())
    return result;

  const auto [par_cnt, brk_cnt] = countParenAndBracketEdges(graph);
  const DyckProjectionKind first_kind = brk_cnt <= par_cnt
                                            ? DyckProjectionKind::Brackets
                                            : DyckProjectionKind::Parentheses;
  const DyckProjectionKind second_kind = brk_cnt <= par_cnt
                                             ? DyckProjectionKind::Parentheses
                                             : DyckProjectionKind::Brackets;

  const IndexedVertices ig(graph);
  const auto sys = condenseDyckGraph(graph, ig, first_kind);

  std::vector<std::vector<std::uint32_t>> members(sys.cur_v);
  for (std::uint32_t i = 0; i < ig.size(); ++i)
    members[sys.rep[i]].push_back(i);

  PairSet first_non_self;
  for (const auto &group : members) {
    for (std::uint32_t u : group) {
      for (std::uint32_t v : group) {
        if (u != v)
          first_non_self.insert({ig.verts[u], ig.verts[v]});
      }
    }
  }

  // Only condensed vertices with at least one outgoing edge can reach other
  // condensed components.
  std::vector<std::uint8_t> has_out(sys.cur_v, 0);
  std::vector<std::uint32_t> active_seeds;
  for (const auto &e : sys.edges) {
    if (!has_out[e.u]) {
      has_out[e.u] = 1;
      active_seeds.push_back(e.u);
    }
  }

  std::vector<std::vector<std::uint32_t>> ord_adj(sys.cur_v);
  for (const auto &e : sys.edges)
    ord_adj[e.u].push_back(e.v);
  const SccDag ord_dag = buildSccDag(sys.cur_v, ord_adj);

  std::vector<std::pair<std::uint32_t, std::uint32_t>> cand_cond_pairs;
  std::vector<std::uint64_t> reach_mask(ord_dag.num_sccs, 0ULL);
  for (std::size_t base = 0; base < active_seeds.size(); base += 64) {
    const std::size_t batch =
        std::min<std::size_t>(64, active_seeds.size() - base);
    std::fill(reach_mask.begin(), reach_mask.end(), 0ULL);
    for (std::size_t bit_idx = 0; bit_idx < batch; ++bit_idx)
      reach_mask[ord_dag.scc_of[active_seeds[base + bit_idx]]] |=
          (1ULL << bit_idx);
    for (std::uint32_t cu : ord_dag.topo) {
      const std::uint64_t m = reach_mask[cu];
      if (!m)
        continue;
      for (std::uint32_t cv : ord_dag.dag_fwd[cu])
        reach_mask[cv] |= m;
    }
    for (std::uint32_t target_c = 0; target_c < sys.cur_v; ++target_c) {
      if (members[target_c].empty())
        continue;
      const std::uint64_t m = reach_mask[ord_dag.scc_of[target_c]];
      if (!m)
        continue;
      for (std::size_t bit_idx = 0; bit_idx < batch; ++bit_idx) {
        if ((m & (1ULL << bit_idx)) != 0ULL) {
          const std::uint32_t src_c = active_seeds[base + bit_idx];
          if (src_c != target_c)
            cand_cond_pairs.emplace_back(src_c, target_c);
        }
      }
    }
  }

  if (!cand_cond_pairs.empty()) {
    std::vector<std::uint32_t> uniq_seeds;
    uniq_seeds.reserve(cand_cond_pairs.size());
    for (const auto &[sc, tc] : cand_cond_pairs)
      uniq_seeds.push_back(sc);
    std::sort(uniq_seeds.begin(), uniq_seeds.end());
    uniq_seeds.erase(std::unique(uniq_seeds.begin(), uniq_seeds.end()),
                     uniq_seeds.end());

    runSharedRhsTabulation(
        sys.cur_v, sys.edges, false, uniq_seeds, cand_cond_pairs,
        [&](std::size_t qi) {
          const auto [sc, tc] = cand_cond_pairs[qi];
          for (std::uint32_t u : members[sc]) {
            for (std::uint32_t v : members[tc])
              first_non_self.insert({ig.verts[u], ig.verts[v]});
          }
        });
  }

  if (first_non_self.empty())
    return result;

  const Graph sliced = sliceGraphOnCandidatePaths(graph, first_non_self);
  const std::vector<Pair> survivors(first_non_self.begin(),
                                    first_non_self.end());
  PairSet second_kept =
      filterDyckReachableDemands(sliced, survivors, second_kind, false);
  for (const Pair &p : result)
    second_kept.insert(p);
  return second_kept;
}

StrongComponents computeStrongComponents(
    std::uint32_t n, const std::vector<std::vector<std::uint32_t>> &adj) {
  auto res = computeIterativeTarjanScc(n, adj);
  return {res.num_sccs, std::move(res.scc_of)};
}

Graph sliceGraphOnCandidatePaths(const Graph &graph, const PairSet &pairs) {
  if (pairs.empty() || graph.empty())
    return {};
  const std::vector<Pair> pair_vec(pairs.begin(), pairs.end());
  return sliceGraphImpl(graph, pair_vec, /*ignore_self_pairs=*/false, nullptr,
                        nullptr);
}

Graph sliceGraphForDemands(const Graph &graph, const std::vector<Pair> &pairs,
                           const std::vector<Vertex> &sources,
                           const std::vector<Vertex> &targets) {
  if (graph.empty())
    return {};
  return sliceGraphImpl(graph, pairs, /*ignore_self_pairs=*/true, &sources,
                        &targets);
}

} // namespace lotus::cfl::interleaved_dyck::spds
