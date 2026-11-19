#include "CFL/InterleavedDyck/AffineSPDS/Solver.h"
#include "CFL/InterleavedDyck/AffineSPDS/ParitySemiring.h"
#include "CFL/InterleavedDyck/SPDS/DyckReachability.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>

namespace lotus::cfl::interleaved_dyck::affine {
namespace {
using Clock = std::chrono::steady_clock;
using spds::State;
using spds::Symbol;
using System = spds::PushdownSystem<AffineSemiring>;
using Automaton = spds::Automaton<AffineSemiring>;
using BooleanSystem = spds::PushdownSystem<spds::BooleanSemiring>;
constexpr State Missing = std::numeric_limits<State>::max();
std::uint64_t elapsed(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                               start)
      .count();
}
Symbol symbol(unsigned id) { return static_cast<Symbol>(id) + 1; }
std::vector<Symbol> word(const std::vector<unsigned> &ids) {
  std::vector<Symbol> result;
  result.reserve(ids.size() + 1);
  for (unsigned id : ids)
    result.push_back(symbol(id));
  result.push_back(0);
  return result;
}
void addTimings(spds::Statistics &a, const spds::Statistics &b) {
  a.setup_microseconds += b.setup_microseconds;
  a.saturation_microseconds += b.saturation_microseconds;
  a.readout_microseconds += b.readout_microseconds;
  a.projection_microseconds += b.projection_microseconds;
}
void add(spds::Statistics &a, const spds::Statistics &b) {
  a.states += b.states;
  a.transitions += b.transitions;
  a.updates += b.updates;
  a.processed += b.processed;
  a.rules += b.rules;
  addTimings(a, b);
}
void add(Statistics &a, const Statistics &b) {
  add(a.saturation, b.saturation);
  a.slice_cache_hits += b.slice_cache_hits;
  a.compiled_rules += b.compiled_rules;
  a.prepared_weights += b.prepared_weights;
  a.maximum_affine_rank =
      std::max(a.maximum_affine_rank, b.maximum_affine_rank);
  a.algebra.matrix_products += b.algebra.matrix_products;
  a.algebra.basis_reductions += b.algebra.basis_reductions;
  a.algebra.basis_insertions += b.algebra.basis_insertions;
  a.algebra.cow_detaches += b.algebra.cow_detaches;
  a.algebra.intersection_tests += b.algebra.intersection_tests;
  a.algebra.intersection_fast_paths += b.algebra.intersection_fast_paths;
  a.algebra.intersection_us += b.algebra.intersection_us;
  a.algebra.certificate_us += b.algebra.certificate_us;
}
bool selected(const HistoryComparison &c, const HistoryObserver &o,
              ComparisonMode mode) {
  switch (mode) {
  case ComparisonMode::Joint:
    return c.mayReach();
  case ComparisonMode::Independent:
    return c.independentMayReach(o);
  case ComparisonMode::Projection:
    return c.spdsMayReach();
  }
  throw std::invalid_argument("invalid AffineSPDS comparison mode");
}
Automaton saturate(const System &system, State anchor,
                   spds::Direction direction, spds::Limits limits,
                   std::shared_ptr<AlgebraStatistics> statistics) {
  auto seed = spds::RegularSet::singleton(system.controls(), {anchor, {0}});
  spds::SaturationSession<AffineSemiring> session(
      system, seed, direction, limits,
      system.domain().withStatistics(std::move(statistics)));
  session.run();
  return session.takeResult();
}
Automaton emptyAutomaton(const AffineSemiring &domain, std::size_t controls,
                         spds::Direction direction,
                         std::shared_ptr<AlgebraStatistics> statistics) {
  System system(domain.withStatistics(std::move(statistics)));
  for (std::size_t i = 0; i < controls; ++i)
    system.addControl();
  spds::RegularSet seed(controls);
  spds::SaturationSession<AffineSemiring> session(system, seed, direction);
  session.run();
  return session.takeResult();
}
void addBooleanRule(BooleanSystem &system, const Edge &edge, State from,
                    State to, bool call) {
  const auto open =
      call ? LabelKind::OpenParenthesis : LabelKind::OpenBracket;
  const auto close =
      call ? LabelKind::CloseParenthesis : LabelKind::CloseBracket;
  if (edge.label.kind == close)
    system.addRule(from, symbol(edge.label.id), to, {});
  else if (edge.label.kind == open)
    system.addPushRule(from, to, symbol(edge.label.id));
  else
    system.addPreserveRule(from, to);
}
bool hasUnboundedLimits(const spds::Limits &l) {
  return l.max_states == 0 && l.max_transitions == 0 && l.max_updates == 0;
}
struct CondensedPdsRule {
  std::uint32_t cu;
  std::uint32_t cv;
  std::uint8_t tag;
  std::size_t param;
  bool operator<(const CondensedPdsRule &o) const {
    return std::tie(cu, cv, tag, param) <
           std::tie(o.cu, o.cv, o.tag, o.param);
  }
  bool operator==(const CondensedPdsRule &o) const {
    return cu == o.cu && cv == o.cv && tag == o.tag && param == o.param;
  }
};
void accumulateAlgebraStats(AlgebraStatistics &dst,
                            const AlgebraStatistics &src) {
  dst.matrix_products += src.matrix_products;
  dst.basis_reductions += src.basis_reductions;
  dst.basis_insertions += src.basis_insertions;
  dst.cow_detaches += src.cow_detaches;
  dst.intersection_tests += src.intersection_tests;
  dst.intersection_fast_paths += src.intersection_fast_paths;
  dst.intersection_us += src.intersection_us;
  dst.certificate_us += src.certificate_us;
}
} // namespace

HistoryComparison::HistoryComparison(
    AffineSpace calls, AffineSpace fields,
    std::shared_ptr<AlgebraStatistics> statistics)
    : calls_(std::move(calls)), fields_(std::move(fields)),
      statistics_(std::move(statistics)) {
  if (calls_.dimension() != fields_.dimension())
    throw std::invalid_argument("history dimension mismatch");
}
Verdict HistoryComparison::verdict() const {
  if (!verdict_) {
    const auto started = Clock::now();
    const auto certificate_before =
        statistics_ ? statistics_->certificate_us : 0;
    if (!spdsMayReach()) {
      verdict_ = Verdict::ProjectionRejected;
      certificate_.emplace(std::nullopt);
    } else {
      // Keep the small separating equation, not an entire factorization.
      // A later certificate() call performs no second elimination.
      certificate_ = separate(calls_, fields_, statistics_.get());
      verdict_ = *certificate_ ? Verdict::AffineSeparated : Verdict::MayReach;
    }
    if (statistics_)
      statistics_->intersection_us +=
          elapsed(started) - (statistics_->certificate_us - certificate_before);
  }
  return *verdict_;
}
bool HistoryComparison::mayReach() const {
  return verdict() == Verdict::MayReach;
}
const std::optional<SeparationCertificate> &
HistoryComparison::certificate() const {
  (void)verdict();
  return *certificate_;
}
bool HistoryComparison::independentMayReach(
    const HistoryObserver &observer) const {
  if (observer.dimension() != calls_.dimension())
    throw std::invalid_argument("observer/history mismatch");
  if (independent_ && independent_->first == observer.blocks())
    return independent_->second;
  const auto start = Clock::now();
  bool result = spdsMayReach();
  if (result)
    for (const auto &block : observer.blocks())
      if (!calls_.block(block.first, block.second)
               .intersects(fields_.block(block.first, block.second),
                           statistics_.get())) {
        result = false;
        break;
      }
  independent_ = std::make_pair(observer.blocks(), result);
  if (statistics_)
    statistics_->intersection_us += elapsed(start);
  return result;
}
HistoryComparison synchronize(const Automaton &calls,
                              const spds::Configuration &call_query,
                              const Automaton &fields,
                              const spds::Configuration &field_query) {
  if (calls.direction() != fields.direction())
    throw std::invalid_argument("mixed post/pre synchronization");
  return HistoryComparison(
      calls.weight(call_query.control, call_query.stack),
      fields.weight(field_query.control, field_query.stack),
      calls.domain().statistics());
}

QueryResult::QueryResult(
    Vertex anchor, spds::Direction direction, Options options,
    std::shared_ptr<const std::map<Vertex, State>> controls,
    std::shared_ptr<const HistoryObserver> observer, Automaton calls,
    Automaton fields)
    : anchor_(anchor), direction_(direction), options_(options),
      controls_(std::move(controls)), observer_(std::move(observer)),
      calls_(std::move(calls)), fields_(std::move(fields)) {
  statistics_.matrix_dimension = observer_->dimension();
  statistics_.coordinate_dimension = calls_.domain().coordinates();
  for (const auto *automaton : {&calls_, &fields_}) {
    add(statistics_.saturation, automaton->statistics());
    for (const auto &entry : automaton->transitions())
      statistics_.maximum_affine_rank =
          std::max(statistics_.maximum_affine_rank, entry.weight.rank());
  }
}
Statistics QueryResult::statistics() const {
  auto result = statistics_;
  result.saturation.readout_microseconds +=
      calls_.statistics().readout_microseconds +
      fields_.statistics().readout_microseconds;
  if (calls_.domain().statistics())
    result.algebra = *calls_.domain().statistics();
  return result;
}
void QueryResult::prepareReadout() const {
  if (call_weights_ && field_weights_)
    return;
  if (!call_weights_)
    call_weights_ = options_.parentheses == spds::StackAcceptance::Empty
                        ? calls_.controlWeights({0})
                        : calls_.controlWeightsWithPrefix({});
  if (!field_weights_)
    field_weights_ = options_.brackets == spds::StackAcceptance::Empty
                         ? fields_.controlWeights({0})
                         : fields_.controlWeightsWithPrefix({});
  for (const auto *weights : {&*call_weights_, &*field_weights_})
    for (const auto &weight : *weights)
      statistics_.maximum_affine_rank =
          std::max(statistics_.maximum_affine_rank, weight.rank());
}
AffineSpace QueryResult::history(const Automaton &automaton, Vertex vertex,
                                 spds::StackAcceptance acceptance) const {
  const auto found = controls_->find(vertex);
  if (found == controls_->end())
    return automaton.domain().zero();
  const auto &weights = &automaton == &calls_ ? call_weights_ : field_weights_;
  if (weights)
    return (*weights)[found->second];
  return acceptance == spds::StackAcceptance::Empty
             ? automaton.weight(found->second, {0})
             : automaton.weightWithPrefix(found->second, {});
}
const HistoryComparison &QueryResult::compare(Vertex vertex) const {
  if (!controls_->count(vertex)) {
    if (!missing_)
      missing_.emplace(calls_.domain().zero(), fields_.domain().zero());
    return *missing_;
  }
  auto found = cache_.find(vertex);
  if (found == cache_.end()) {
    if (options_.readout_batch_threshold &&
        cache_.size() >= options_.readout_batch_threshold)
      prepareReadout();
    found =
        cache_
            .emplace(vertex, HistoryComparison(
                                 history(calls_, vertex, options_.parentheses),
                                 history(fields_, vertex, options_.brackets),
                                 calls_.domain().statistics()))
            .first;
    statistics_.maximum_affine_rank = std::max(
        {statistics_.maximum_affine_rank, found->second.callHistory().rank(),
         found->second.fieldHistory().rank()});
  }
  return found->second;
}
HistoryComparison
QueryResult::compareStacks(Vertex vertex, const std::vector<unsigned> &calls,
                           const std::vector<unsigned> &fields) const {
  const auto found = controls_->find(vertex);
  if (found == controls_->end())
    return HistoryComparison(calls_.domain().zero(), fields_.domain().zero());
  const StackKey key{vertex, calls, fields};
  if (const auto cached = stack_cache_.find(key); cached != stack_cache_.end())
    return cached->second;
  auto result = synchronize(calls_, {found->second, word(calls)}, fields_,
                            {found->second, word(fields)});
  statistics_.maximum_affine_rank =
      std::max({statistics_.maximum_affine_rank, result.callHistory().rank(),
                result.fieldHistory().rank()});
  if (stack_cache_.size() == 64)
    stack_cache_.erase(stack_cache_.begin());
  stack_cache_.emplace(key, result);
  return result;
}

struct PreparedAnalysis::Storage {
  struct DenseEdge {
    State from, to;
  };
  struct WeightedEdge {
    AffineSpace weight;
    AffineSemiring::PreparedWeight prepared;
  };
  struct Projection {
    explicit Projection(const AffineSemiring &domain)
        : calls(domain), fields(domain) {}
    std::shared_ptr<const std::map<Vertex, State>> controls;
    System calls, fields;
  };
  Options options;
  std::shared_ptr<const HistoryObserver> observer;
  AffineSemiring domain;
  std::map<Vertex, State> controls;
  std::vector<Vertex> vertices;
  std::vector<Edge> edges;
  std::vector<DenseEdge> dense_edges;
  std::vector<std::vector<std::size_t>> outgoing, incoming;
  std::vector<State> scc;
  std::vector<std::pair<State, State>> non_identity_edges;
  bool is_identity = true;
  std::size_t pure_parity_bits = 0;
  std::vector<std::uint64_t> edge_parity_masks;
  std::uint64_t preparation_us = 0, observer_us = 0;
  // Only slice compilation touches these caches. Independent saturations and
  // readouts have distinct counters and run outside this lock.
  mutable std::mutex mutex;
  mutable std::unordered_map<std::size_t, std::shared_ptr<const WeightedEdge>>
      weights;
  mutable std::shared_ptr<const WeightedEdge> identity_weight;
  mutable std::map<std::pair<spds::Direction, State>,
                   std::shared_ptr<const Projection>>
      slices;
  mutable std::deque<std::pair<spds::Direction, State>> order;
  mutable std::size_t cached_rules = 0;

  Storage(Options opts, const Graph &graph,
          std::shared_ptr<const HistoryObserver> obs)
      : options(opts), observer(std::move(obs)),
        domain(observer->dimension(),
               options.compress_observer ? observer->compactLayout() : nullptr),
        vertices(graph.vertices()), edges(graph.edges()) {
    std::sort(vertices.begin(), vertices.end());
    std::sort(edges.begin(), edges.end(), EdgeLess{});
    outgoing.resize(vertices.size());
    incoming.resize(vertices.size());
    for (State v = 0; v < vertices.size(); ++v)
      controls.emplace(vertices[v], v);
    for (const auto &edge : edges) {
      const auto from = controls.at(edge.source), to = controls.at(edge.target);
      const auto id = dense_edges.size();
      dense_edges.push_back({from, to});
      outgoing[from].push_back(id);
      incoming[to].push_back(id);
    }
    // Iterative Kosaraju: SCC members share each directional structural slice.
    std::vector<bool> seen(vertices.size(), false);
    std::vector<State> finish;
    for (State root = 0; root < vertices.size(); ++root) {
      if (seen[root])
        continue;
      seen[root] = true;
      std::vector<std::pair<State, std::size_t>> stack{{root, 0}};
      while (!stack.empty()) {
        auto &frame = stack.back();
        if (frame.second == outgoing[frame.first].size()) {
          finish.push_back(frame.first);
          stack.pop_back();
          continue;
        }
        const State next =
            dense_edges[outgoing[frame.first][frame.second++]].to;
        if (!seen[next]) {
          seen[next] = true;
          stack.emplace_back(next, 0);
        }
      }
    }
    scc.assign(vertices.size(), Missing);
    for (auto root = finish.rbegin(); root != finish.rend(); ++root) {
      if (scc[*root] != Missing)
        continue;
      std::vector<State> work{*root};
      scc[*root] = *root;
      for (std::size_t cursor = 0; cursor < work.size(); ++cursor)
        for (auto id : incoming[work[cursor]]) {
          const auto next = dense_edges[id].from;
          if (scc[next] == Missing) {
            scc[next] = *root;
            work.push_back(next);
          }
        }
    }
    if (!observer->assignments().empty()) {
      for (std::size_t id = 0; id < edges.size(); ++id) {
        if (!observer->matrix(edges[id]).isIdentity()) {
          non_identity_edges.emplace_back(dense_edges[id].from,
                                          dense_edges[id].to);
          is_identity = false;
        }
      }
    }
    const auto &obs_blocks = observer->blocks();
    const std::size_t bits = obs_blocks.size();
    bool ok_parity = options.compress_observer && !is_identity && bits >= 1U &&
                     bits <= 64U && observer->dimension() == bits * 2U &&
                     (bits == 1U || domain.layout() != nullptr);
    if (ok_parity) {
      for (std::size_t b = 0; b < bits; ++b) {
        if (obs_blocks[b].first != 2U * b || obs_blocks[b].second != 2U) {
          ok_parity = false;
          break;
        }
      }
    }
    if (ok_parity) {
      for (const auto &entry : observer->assignments()) {
        const Matrix &mat = entry.second;
        for (std::size_t b = 0; b < bits; ++b) {
          if (!mat.get(2U * b, 2U * b) || mat.get(2U * b + 1U, 2U * b) ||
              !mat.get(2U * b + 1U, 2U * b + 1U)) {
            ok_parity = false;
            break;
          }
        }
        if (!ok_parity)
          break;
      }
    }
    if (ok_parity) {
      pure_parity_bits = bits;
      edge_parity_masks.assign(edges.size(), 0);
      for (std::size_t id = 0; id < edges.size(); ++id) {
        const Matrix &mat = observer->matrix(edges[id]);
        if (mat.isIdentity())
          continue;
        std::uint64_t mask = 0;
        for (std::size_t b = 0; b < bits; ++b) {
          if (mat.get(2U * b, 2U * b + 1U))
            mask |= (std::uint64_t{1} << b);
        }
        edge_parity_masks[id] = mask;
      }
    }
  }
  bool sliceIsIdentity(const std::vector<bool> &live) const {
    if (is_identity)
      return true;
    for (const auto &edge : non_identity_edges)
      if (live[edge.first] && live[edge.second])
        return false;
    return true;
  }
  struct AnchorInfo {
    std::size_t usable_edges = 0;
    bool reenterable = false;
  };
  struct BooleanFilter {
    std::vector<Vertex> surviving;
    spds::Statistics statistics;
    bool corridor_identity = true;
  };
  AnchorInfo inspectAnchor(State start, spds::Direction direction, bool call,
                           const std::vector<bool> &live) const {
    AnchorInfo info;
    const auto blocked_kind =
        direction == spds::Direction::Post
            ? (call ? LabelKind::CloseParenthesis : LabelKind::CloseBracket)
            : (call ? LabelKind::OpenParenthesis : LabelKind::OpenBracket);
    if (direction == spds::Direction::Post) {
      for (auto id : outgoing[start])
        if (live[dense_edges[id].to] && edges[id].label.kind != blocked_kind)
          ++info.usable_edges;
      for (auto id : incoming[start])
        if (live[dense_edges[id].from]) {
          info.reenterable = true;
          break;
        }
    } else {
      for (auto id : incoming[start])
        if (live[dense_edges[id].from] && edges[id].label.kind != blocked_kind)
          ++info.usable_edges;
      for (auto id : outgoing[start])
        if (live[dense_edges[id].to]) {
          info.reenterable = true;
          break;
        }
    }
    return info;
  }
  std::vector<State>
  runBooleanProjection(State start, spds::Direction direction, bool call,
                       const std::vector<bool> &live, bool reenterable,
                       const std::vector<State> &targets,
                       const std::vector<unsigned> *explicit_stack,
                       spds::Statistics &stats) const {
    const auto proj_started = Clock::now();
    std::vector<State> indices(vertices.size(), Missing);
    std::vector<State> local_vertices;
    BooleanSystem system;
    for (State v = 0; v < vertices.size(); ++v)
      if (live[v]) {
        indices[v] = system.addControl();
        local_vertices.push_back(v);
      }
    const auto blocked_kind =
        direction == spds::Direction::Post
            ? (call ? LabelKind::CloseParenthesis : LabelKind::CloseBracket)
            : (call ? LabelKind::OpenParenthesis : LabelKind::OpenBracket);
    const auto acceptance = call ? options.parentheses : options.brackets;
    State single_end = Missing;
    LabelKind end_blocked_kind = LabelKind::Neutral;
    if (targets.size() == 1 && targets[0] != start && !explicit_stack &&
        acceptance == spds::StackAcceptance::Empty) {
      const spds::Direction opp = direction == spds::Direction::Post
                                      ? spds::Direction::Pre
                                      : spds::Direction::Post;
      if (!inspectAnchor(targets[0], opp, call, live).reenterable) {
        single_end = targets[0];
        end_blocked_kind =
            opp == spds::Direction::Post
                ? (call ? LabelKind::CloseParenthesis : LabelKind::CloseBracket)
                : (call ? LabelKind::OpenParenthesis : LabelKind::OpenBracket);
      }
    }
    for (State u : local_vertices)
      for (auto id : outgoing[u]) {
        const State v = dense_edges[id].to;
        if (!live[v])
          continue;
        if (!reenterable) {
          if (direction == spds::Direction::Post && u == start &&
              edges[id].label.kind == blocked_kind)
            continue;
          if (direction == spds::Direction::Pre && v == start &&
              edges[id].label.kind == blocked_kind)
            continue;
        }
        if (single_end != Missing) {
          if (direction == spds::Direction::Post && v == single_end &&
              edges[id].label.kind == end_blocked_kind)
            continue;
          if (direction == spds::Direction::Pre && u == single_end &&
              edges[id].label.kind == end_blocked_kind)
            continue;
        }
        addBooleanRule(system, edges[id], indices[u], indices[v], call);
      }
    const auto proj_us = elapsed(proj_started);
    auto seed =
        spds::RegularSet::singleton(system.controls(), {indices[start], {0}});
    auto automaton =
        direction == spds::Direction::Post
            ? spds::postStar(system, seed, options.limits)
            : spds::preStar(system, seed, options.limits);
    std::vector<State> surviving;
    const std::size_t batch_threshold =
        options.readout_batch_threshold ? options.readout_batch_threshold : 8;
    if (explicit_stack) {
      const auto w = word(*explicit_stack);
      for (State t : targets)
        if (automaton.accepts(indices[t], w))
          surviving.push_back(t);
    } else if (targets.size() >= batch_threshold) {
      const auto weights =
          acceptance == spds::StackAcceptance::Empty
              ? automaton.controlWeights({0})
              : automaton.controlWeightsWithPrefix({});
      for (State t : targets)
        if (weights[indices[t]])
          surviving.push_back(t);
    } else {
      for (State t : targets) {
        const bool ok =
            acceptance == spds::StackAcceptance::Empty
                ? automaton.accepts(indices[t], {0})
                : automaton.acceptsPrefix(indices[t], {});
        if (ok)
          surviving.push_back(t);
      }
    }
    add(stats, automaton.statistics());
    stats.projection_microseconds += proj_us;
    return surviving;
  }
  BooleanFilter filterBoolean(
      Vertex anchor, spds::Direction direction,
      const std::vector<Vertex> *candidates,
      const std::vector<unsigned> *call_stack = nullptr,
      const std::vector<unsigned> *field_stack = nullptr) const {
    BooleanFilter result;
    const auto reach_started = Clock::now();
    const State start = controls.at(anchor);
    if (candidates && candidates->size() == 1) {
      const State cand = controls.at(candidates->front());
      if (cand != start) {
        const bool has_out = direction == spds::Direction::Post
                                 ? !outgoing[start].empty()
                                 : !incoming[start].empty();
        const bool has_in = direction == spds::Direction::Post
                                ? !incoming[cand].empty()
                                : !outgoing[cand].empty();
        if (!has_out || !has_in) {
          result.statistics.projection_microseconds += elapsed(reach_started);
          return result;
        }
      }
    }
    const auto fwd = reachable({start}, direction);
    std::vector<State> targets;
    if (!candidates) {
      for (State v = 0; v < vertices.size(); ++v)
        if (v != start && fwd[v])
          targets.push_back(v);
    } else {
      std::vector<bool> seen(vertices.size(), false);
      for (Vertex v : *candidates) {
        const State s = controls.at(v);
        if (fwd[s] && !seen[s]) {
          seen[s] = true;
          targets.push_back(s);
        }
      }
    }
    if (targets.empty()) {
      result.statistics.projection_microseconds += elapsed(reach_started);
      return result;
    }
    if (!call_stack && !field_stack && targets.size() == 1 &&
        targets[0] == start) {
      const auto rev_self = reachable(
          targets, direction == spds::Direction::Post ? spds::Direction::Pre
                                                      : spds::Direction::Post);
      std::vector<bool> self_live(vertices.size(), false);
      for (State v = 0; v < vertices.size(); ++v)
        self_live[v] = fwd[v] && rev_self[v];
      result.corridor_identity = sliceIsIdentity(self_live);
      result.statistics.projection_microseconds += elapsed(reach_started);
      result.surviving.push_back(anchor);
      return result;
    }
    std::vector<bool> live;
    if (!candidates) {
      live = fwd;
    } else {
      const auto rev = reachable(
          targets, direction == spds::Direction::Post ? spds::Direction::Pre
                                                      : spds::Direction::Post);
      live.resize(vertices.size(), false);
      for (State v = 0; v < vertices.size(); ++v)
        live[v] = fwd[v] && rev[v];
      live[start] = true;
    }
    result.statistics.projection_microseconds += elapsed(reach_started);

    const auto call_info = inspectAnchor(start, direction, true, live);
    const auto field_info = inspectAnchor(start, direction, false, live);

    auto checkEmptyAnchorProj = [&](bool is_call, const AnchorInfo &info) {
      if (info.usable_edges != 0)
        return false;
      const auto *stack = is_call ? call_stack : field_stack;
      const bool accepts_empty = !stack || stack->empty();
      if (!accepts_empty ||
          std::find(targets.begin(), targets.end(), start) == targets.end()) {
        targets.clear();
      } else {
        targets = {start};
      }
      return true;
    };

    // For a single non-self endpoint under Empty acceptance, an empty-stack
    // path must also enter/leave the opposite endpoint validly.
    if (candidates && targets.size() == 1 && targets[0] != start &&
        !call_stack && !field_stack) {
      const spds::Direction opp = direction == spds::Direction::Post
                                      ? spds::Direction::Pre
                                      : spds::Direction::Post;
      if ((options.parentheses == spds::StackAcceptance::Empty ||
           direction == spds::Direction::Pre) &&
          inspectAnchor(targets[0], opp, true, live).usable_edges == 0)
        return result;
      if ((options.brackets == spds::StackAcceptance::Empty ||
           direction == spds::Direction::Pre) &&
          inspectAnchor(targets[0], opp, false, live).usable_edges == 0)
        return result;
    }

    const bool first_call = call_info.usable_edges <= field_info.usable_edges;
    const auto &first_info = first_call ? call_info : field_info;
    const auto &second_info = first_call ? field_info : call_info;
    const auto *first_stack = first_call ? call_stack : field_stack;
    const auto *second_stack = first_call ? field_stack : call_stack;

    const std::size_t prev_targets = targets.size();
    if (!checkEmptyAnchorProj(first_call, first_info)) {
      targets = runBooleanProjection(start, direction, first_call, live,
                                     first_info.reenterable, targets,
                                     first_stack, result.statistics);
    }
    if (targets.empty())
      return result;
    if (!call_stack && !field_stack && targets.size() == 1 &&
        targets[0] == start) {
      const auto rev_self = reachable(
          targets, direction == spds::Direction::Post ? spds::Direction::Pre
                                                      : spds::Direction::Post);
      std::vector<bool> self_live(vertices.size(), false);
      for (State v = 0; v < vertices.size(); ++v)
        self_live[v] = fwd[v] && rev_self[v];
      result.corridor_identity = sliceIsIdentity(self_live);
      result.surviving.push_back(anchor);
      return result;
    }

    const bool need_reslice = targets.size() < prev_targets;
    AnchorInfo resliced_second = second_info;
    if (need_reslice) {
      const auto reslice_started = Clock::now();
      const auto rev2 = reachable(
          targets, direction == spds::Direction::Post ? spds::Direction::Pre
                                                      : spds::Direction::Post);
      for (State v = 0; v < vertices.size(); ++v)
        live[v] = fwd[v] && rev2[v];
      live[start] = true;
      resliced_second = inspectAnchor(start, direction, !first_call, live);
      result.statistics.projection_microseconds += elapsed(reslice_started);
    }

    const std::size_t prev_targets2 = targets.size();
    if (!checkEmptyAnchorProj(!first_call, resliced_second)) {
      targets = runBooleanProjection(start, direction, !first_call, live,
                                     resliced_second.reenterable, targets,
                                     second_stack, result.statistics);
    }
    if (targets.empty())
      return result;
    if (!is_identity) {
      if (targets.size() < prev_targets2) {
        const auto reslice_started = Clock::now();
        const auto rev3 = reachable(
            targets, direction == spds::Direction::Post ? spds::Direction::Pre
                                                        : spds::Direction::Post);
        for (State v = 0; v < vertices.size(); ++v)
          live[v] = fwd[v] && rev3[v];
        live[start] = true;
        result.statistics.projection_microseconds += elapsed(reslice_started);
      }
      result.corridor_identity = sliceIsIdentity(live);
    }
    for (State s : targets)
      result.surviving.push_back(vertices[s]);
    return result;
  }
  std::vector<bool> reachable(const std::vector<State> &seeds,
                              spds::Direction direction) const {
    const auto &adjacency =
        direction == spds::Direction::Post ? outgoing : incoming;
    std::vector<bool> live(vertices.size(), false);
    std::vector<State> work;
    for (State seed : seeds)
      if (!live[seed]) {
        live[seed] = true;
        work.push_back(seed);
      }
    for (std::size_t cursor = 0; cursor < work.size(); ++cursor)
      for (auto id : adjacency[work[cursor]]) {
        const auto next = direction == spds::Direction::Post
                              ? dense_edges[id].to
                              : dense_edges[id].from;
        if (!live[next]) {
          live[next] = true;
          work.push_back(next);
        }
      }
    return live;
  }
  std::shared_ptr<const WeightedEdge> weight(std::size_t id,
                                             Statistics &statistics) const {
    if (const auto found = weights.find(id); found != weights.end())
      return found->second;
    const auto &matrix = observer->matrix(edges[id]);
    if (matrix.isIdentity()) {
      if (!identity_weight) {
        ++statistics.prepared_weights;
        identity_weight = std::make_shared<const WeightedEdge>(
            WeightedEdge{domain.one(), nullptr});
      }
      return identity_weight;
    }
    ++statistics.prepared_weights;
    auto value = domain.lift(matrix);
    auto prepared = domain.prepareWeight(value);
    auto entry = std::make_shared<const WeightedEdge>(
        WeightedEdge{std::move(value), std::move(prepared)});
    weights.emplace(id, entry);
    return entry;
  }
  static System::Rule rule(const Edge &edge, const WeightedEdge &value,
                           bool call) {
    const auto open =
        call ? LabelKind::OpenParenthesis : LabelKind::OpenBracket;
    const auto close =
        call ? LabelKind::CloseParenthesis : LabelKind::CloseBracket;
    if (edge.label.kind == close)
      return {System::RuleKind::Exact,
              0,
              symbol(edge.label.id),
              0,
              {},
              value.weight,
              value.prepared};
    if (edge.label.kind == open)
      return {System::RuleKind::PushAny,
              0,
              0,
              0,
              {symbol(edge.label.id)},
              value.weight,
              value.prepared};
    return {System::RuleKind::PreserveAny,
            0,
            0,
            0,
            {},
            value.weight,
            value.prepared};
  }
  std::shared_ptr<const Projection>
  project(Vertex anchor, spds::Direction direction,
          const std::vector<Vertex> *endpoints, Statistics &statistics) const {
    std::lock_guard<std::mutex> lock(mutex);
    const State start = controls.at(anchor);
    const auto key = std::make_pair(direction, scc[start]);
    if (!endpoints)
      if (const auto found = slices.find(key); found != slices.end()) {
        ++statistics.slice_cache_hits;
        return found->second;
      }
    auto live = reachable({start}, direction);
    if (endpoints) {
      std::vector<State> destinations;
      for (Vertex v : *endpoints)
        destinations.push_back(controls.at(v));
      const auto reverse =
          reachable(destinations, direction == spds::Direction::Post
                                      ? spds::Direction::Pre
                                      : spds::Direction::Post);
      for (State v = 0; v < vertices.size(); ++v)
        live[v] = live[v] && reverse[v];
      live[start] = true;
    }
    auto projection = std::make_shared<Projection>(domain);
    auto local = std::make_shared<std::map<Vertex, State>>();
    std::vector<State> indices(vertices.size(), Missing);
    for (State v = 0; v < vertices.size(); ++v)
      if (live[v]) {
        const auto id = projection->calls.addControl();
        projection->fields.addControl();
        indices[v] = id;
        local->emplace(vertices[v], id);
      }
    projection->controls = local;
    for (State v = 0; v < vertices.size(); ++v)
      if (live[v])
        for (auto id : outgoing[v]) {
          const auto &edge = dense_edges[id];
          if (!live[edge.to])
            continue;
          const auto value = weight(id, statistics);
          projection->calls.addMappedRule(rule(edges[id], *value, true),
                                          indices[edge.from], indices[edge.to]);
          projection->fields.addMappedRule(rule(edges[id], *value, false),
                                           indices[edge.from],
                                           indices[edge.to]);
        }
    const auto count =
        projection->calls.rules().size() + projection->fields.rules().size();
    statistics.compiled_rules += count;
    if (!endpoints && options.max_cached_slices &&
        options.max_cached_slice_rules &&
        count <= options.max_cached_slice_rules) {
      while (!order.empty() &&
             (slices.size() >= options.max_cached_slices ||
              count > options.max_cached_slice_rules - cached_rules)) {
        const auto oldest = order.front();
        order.pop_front();
        const auto &p = slices.at(oldest);
        cached_rules -= p->calls.rules().size() + p->fields.rules().size();
        slices.erase(oldest);
      }
      slices.emplace(key, projection);
      order.push_back(key);
      cached_rules += count;
    }
    return projection;
  }

  std::vector<bool> corridorLive(State start, spds::Direction direction,
                                 const std::vector<State> &targets) const {
    const auto fwd = reachable({start}, direction);
    const auto rev =
        reachable(targets, direction == spds::Direction::Post
                               ? spds::Direction::Pre
                               : spds::Direction::Post);
    std::vector<bool> live(vertices.size(), false);
    for (State v = 0; v < vertices.size(); ++v)
      live[v] = fwd[v] && rev[v];
    live[start] = true;
    return live;
  }

  std::size_t countObservedEdges(const std::vector<bool> &live,
                                 bool observe_brackets) const {
    const auto open =
        observe_brackets ? LabelKind::OpenBracket : LabelKind::OpenParenthesis;
    const auto close = observe_brackets ? LabelKind::CloseBracket
                                        : LabelKind::CloseParenthesis;
    std::size_t count = 0;
    for (State u = 0; u < vertices.size(); ++u) {
      if (!live[u])
        continue;
      for (auto id : outgoing[u]) {
        if (live[dense_edges[id].to] &&
            (edges[id].label.kind == open || edges[id].label.kind == close))
          ++count;
      }
    }
    return count;
  }

  std::vector<State> runSingleSidedSide(State start, spds::Direction direction,
                                        bool call_balanced,
                                        const std::vector<bool> &live,
                                        const std::vector<State> &targets,
                                        SingleSidedParityOptions parity,
                                        Statistics &stats) const {
    if (targets.empty() || parity.mode == ComparisonMode::Projection)
      return targets;
    const bool observe_brackets = call_balanced;
    const auto bal_open =
        call_balanced ? LabelKind::OpenParenthesis : LabelKind::OpenBracket;
    const auto bal_close =
        call_balanced ? LabelKind::CloseParenthesis : LabelKind::CloseBracket;
    const auto obs_open =
        observe_brackets ? LabelKind::OpenBracket : LabelKind::OpenParenthesis;
    const auto obs_close = observe_brackets ? LabelKind::CloseBracket
                                            : LabelKind::CloseParenthesis;

    std::unordered_map<unsigned, std::size_t> obs_block_map;
    std::unordered_set<unsigned> matched_bal_ids;
    std::size_t eff_groups = parity.groups;

    if (parity.corridor_active_rehash) {
      std::unordered_set<unsigned> o_open, o_close, b_open, b_close;
      for (State u = 0; u < vertices.size(); ++u) {
        if (!live[u])
          continue;
        for (auto id : outgoing[u]) {
          if (!live[dense_edges[id].to])
            continue;
          const auto &lbl = edges[id].label;
          if (lbl.kind == obs_open)
            o_open.insert(lbl.id);
          else if (lbl.kind == obs_close)
            o_close.insert(lbl.id);
          else if (lbl.kind == bal_open)
            b_open.insert(lbl.id);
          else if (lbl.kind == bal_close)
            b_close.insert(lbl.id);
        }
      }
      for (unsigned id : b_open)
        if (b_close.count(id))
          matched_bal_ids.insert(id);
      std::vector<unsigned> active_obs;
      for (unsigned id : o_open)
        if (o_close.count(id))
          active_obs.push_back(id);
      std::sort(active_obs.begin(), active_obs.end());

      if (active_obs.empty()) {
        // After omitting corridor-unmatched delimiters, no observed edges
        // remain; a fast Boolean projection suffices to preserve soundness.
        if (hasUnboundedLimits(options.limits)) {
          Graph filtered_graph;
          for (State u = 0; u < vertices.size(); ++u) {
            if (live[u])
              filtered_graph.addVertex(vertices[u]);
          }
          for (State u = 0; u < vertices.size(); ++u) {
            if (!live[u])
              continue;
            for (auto id : outgoing[u]) {
              const State v = dense_edges[id].to;
              if (!live[v])
                continue;
              const auto &lbl = edges[id].label;
              if (lbl.kind == obs_open || lbl.kind == obs_close)
                continue;
              if ((lbl.kind == bal_open || lbl.kind == bal_close) &&
                  !matched_bal_ids.count(lbl.id))
                continue;
              filtered_graph.addEdge(vertices[u], vertices[v], lbl);
            }
          }
          std::vector<Pair> dems;
          dems.reserve(targets.size());
          const Vertex anchor_v = vertices[start];
          for (State t : targets) {
            dems.push_back(direction == spds::Direction::Post
                               ? Pair{anchor_v, vertices[t]}
                               : Pair{vertices[t], anchor_v});
          }
          const PairSet kept = spds::filterDyckReachableDemands(
              filtered_graph, dems,
              call_balanced ? spds::DyckProjectionKind::Parentheses
                            : spds::DyckProjectionKind::Brackets,
              /*include_self_pairs=*/true);
          std::vector<State> surv;
          surv.reserve(targets.size());
          for (State t : targets) {
            const Pair p = direction == spds::Direction::Post
                               ? Pair{anchor_v, vertices[t]}
                               : Pair{vertices[t], anchor_v};
            if (kept.count(p) != 0U)
              surv.push_back(t);
          }
          return surv;
        }
        BooleanSystem bool_sys;
        std::vector<State> indices(vertices.size(), Missing);
        for (State u = 0; u < vertices.size(); ++u)
          if (live[u])
            indices[u] = bool_sys.addControl();
        for (State u = 0; u < vertices.size(); ++u) {
          if (!live[u])
            continue;
          for (auto id : outgoing[u]) {
            const State v = dense_edges[id].to;
            if (!live[v])
              continue;
            const auto &lbl = edges[id].label;
            if (lbl.kind == obs_open || lbl.kind == obs_close)
              continue;
            if ((lbl.kind == bal_open || lbl.kind == bal_close) &&
                !matched_bal_ids.count(lbl.id))
              continue;
            addBooleanRule(bool_sys, edges[id], indices[u], indices[v],
                           call_balanced);
          }
        }
        auto seed = spds::RegularSet::singleton(bool_sys.controls(),
                                                {indices[start], {0}});
        auto aut = direction == spds::Direction::Post
                       ? spds::postStar(bool_sys, seed, options.limits)
                       : spds::preStar(bool_sys, seed, options.limits);
        add(stats.saturation, aut.statistics());
        std::vector<State> surv;
        const auto weights = aut.controlWeights({0});
        for (State t : targets)
          if (weights[indices[t]])
            surv.push_back(t);
        return surv;
      }
      eff_groups =
          std::min<std::size_t>(parity.groups, active_obs.size());
      for (std::size_t k = 0; k < active_obs.size(); ++k)
        obs_block_map.emplace(active_obs[k], k % eff_groups);
    } else {
      if (countObservedEdges(live, observe_brackets) == 0)
        return targets;
    }

    const auto comp_started = Clock::now();
    std::vector<std::uint32_t> local_of(
        vertices.size(), std::numeric_limits<std::uint32_t>::max());
    std::vector<State> live_nodes;
    for (State u = 0; u < vertices.size(); ++u) {
      if (live[u]) {
        local_of[u] = static_cast<std::uint32_t>(live_nodes.size());
        live_nodes.push_back(u);
      }
    }
    std::vector<std::vector<std::uint32_t>> neutral_adj(live_nodes.size());
    for (std::uint32_t lu = 0; lu < live_nodes.size(); ++lu) {
      const State u = live_nodes[lu];
      for (auto id : outgoing[u]) {
        const State v = dense_edges[id].to;
        if (!live[v] || edges[id].label.kind != LabelKind::Neutral)
          continue;
        const std::uint32_t lv = local_of[v];
        if (lu != lv)
          neutral_adj[lu].push_back(lv);
      }
    }
    const auto neutral_scc = spds::computeStrongComponents(
        static_cast<std::uint32_t>(live_nodes.size()), neutral_adj);

    std::vector<CondensedPdsRule> condensed_rules;
    for (std::uint32_t lu = 0; lu < live_nodes.size(); ++lu) {
      const State u = live_nodes[lu];
      const std::uint32_t cu = neutral_scc.component_of[lu];
      for (auto id : outgoing[u]) {
        const State v = dense_edges[id].to;
        if (!live[v])
          continue;
        const std::uint32_t cv = neutral_scc.component_of[local_of[v]];
        const auto &edge = edges[id];
        const auto kind = edge.label.kind;
        if (kind == obs_open || kind == obs_close) {
          std::size_t b = 0;
          if (parity.corridor_active_rehash) {
            auto it = obs_block_map.find(edge.label.id);
            if (it == obs_block_map.end())
              continue; // corridor-unmatched observed delimiter
            b = it->second;
          } else {
            b = edge.label.id % eff_groups;
          }
          condensed_rules.push_back({cu, cv, 1, b});
        } else if (kind == bal_open) {
          if (parity.corridor_active_rehash &&
              !matched_bal_ids.count(edge.label.id))
            continue;
          condensed_rules.push_back({cu, cv, 2, edge.label.id});
        } else if (kind == bal_close) {
          if (parity.corridor_active_rehash &&
              !matched_bal_ids.count(edge.label.id))
            continue;
          condensed_rules.push_back({cu, cv, 3, edge.label.id});
        } else {
          if (cu != cv)
            condensed_rules.push_back({cu, cv, 4, 0});
        }
      }
    }
    std::sort(condensed_rules.begin(), condensed_rules.end());
    condensed_rules.erase(
        std::unique(condensed_rules.begin(), condensed_rules.end()),
        condensed_rules.end());

    if (eff_groups <= 8U) {
      return runSingleSidedParityImpl<std::uint8_t>(
          direction, call_balanced, eff_groups, neutral_scc, local_of, start,
          targets, condensed_rules, parity, comp_started, stats);
    }
    if (eff_groups <= 16U) {
      return runSingleSidedParityImpl<std::uint16_t>(
          direction, call_balanced, eff_groups, neutral_scc, local_of, start,
          targets, condensed_rules, parity, comp_started, stats);
    }
    if (eff_groups <= 32U) {
      return runSingleSidedParityImpl<std::uint32_t>(
          direction, call_balanced, eff_groups, neutral_scc, local_of, start,
          targets, condensed_rules, parity, comp_started, stats);
    }
    if (eff_groups <= 64U) {
      return runSingleSidedParityImpl<std::uint64_t>(
          direction, call_balanced, eff_groups, neutral_scc, local_of, start,
          targets, condensed_rules, parity, comp_started, stats);
    }

    const std::size_t dim = eff_groups * 2U;
    std::shared_ptr<const MatrixLayout> layout;
    if (eff_groups >= 2U && options.compress_observer) {
      std::vector<std::pair<std::size_t, std::size_t>> blocks;
      blocks.reserve(eff_groups);
      for (std::size_t b = 0; b < eff_groups; ++b)
        blocks.emplace_back(2U * b, 2U);
      layout = std::make_shared<const MatrixLayout>(dim, blocks);
    }
    auto algebra_stats = std::make_shared<AlgebraStatistics>();
    AffineSemiring side_domain =
        AffineSemiring(dim, layout).withStatistics(algebra_stats);
    System pds(side_domain);
    for (std::uint32_t c = 0; c < neutral_scc.count; ++c)
      pds.addControl();

    const WeightedEdge id_weight{side_domain.one(), nullptr};
    std::vector<WeightedEdge> toggle_weights;
    toggle_weights.reserve(eff_groups);
    for (std::size_t b = 0; b < eff_groups; ++b) {
      Matrix m = Matrix::identity(dim);
      m.set(2U * b, 2U * b + 1U);
      auto lifted = side_domain.lift(m);
      auto prep = side_domain.prepareWeight(lifted);
      toggle_weights.push_back(WeightedEdge{std::move(lifted), std::move(prep)});
    }
    stats.prepared_weights += eff_groups + 1U;

    const Edge synthetic_obs{
        0, 0,
        observe_brackets ? Label::openBracket(0) : Label::openParenthesis(0)};
    const Edge synthetic_neutral{0, 0, Label::neutral()};
    for (const auto &cr : condensed_rules) {
      if (cr.tag == 1) {
        pds.addMappedRule(
            rule(synthetic_obs, toggle_weights[cr.param], call_balanced),
            cr.cu, cr.cv);
      } else if (cr.tag == 2) {
        const unsigned lid = static_cast<unsigned>(cr.param);
        const Edge e{0, 0,
                     call_balanced ? Label::openParenthesis(lid)
                                   : Label::openBracket(lid)};
        pds.addMappedRule(rule(e, id_weight, call_balanced), cr.cu, cr.cv);
      } else if (cr.tag == 3) {
        const unsigned lid = static_cast<unsigned>(cr.param);
        const Edge e{0, 0,
                     call_balanced ? Label::closeParenthesis(lid)
                                   : Label::closeBracket(lid)};
        pds.addMappedRule(rule(e, id_weight, call_balanced), cr.cu, cr.cv);
      } else {
        pds.addMappedRule(rule(synthetic_neutral, id_weight, call_balanced),
                          cr.cu, cr.cv);
      }
    }
    stats.compiled_rules += pds.rules().size();
    stats.saturation.projection_microseconds += elapsed(comp_started);

    const State cond_start = neutral_scc.component_of[local_of[start]];
    auto aut =
        saturate(pds, cond_start, direction, options.limits, algebra_stats);
    add(stats.saturation, aut.statistics());

    const std::size_t batch_threshold =
        options.readout_batch_threshold ? options.readout_batch_threshold : 8;
    std::optional<std::vector<AffineSpace>> batch_weights;
    if (targets.size() >= batch_threshold)
      batch_weights = aut.controlWeights({0});

    const AffineSpace one_full = side_domain.one();
    const AffineSpace one_2x2 = AffineSpace::singleton(Matrix::identity(2));
    std::vector<State> surviving;
    surviving.reserve(targets.size());
    for (State t : targets) {
      const State ct = neutral_scc.component_of[local_of[t]];
      const AffineSpace w =
          batch_weights ? (*batch_weights)[ct] : aut.weight(ct, {0});
      stats.maximum_affine_rank = std::max(stats.maximum_affine_rank, w.rank());
      bool ok = false;
      if (parity.mode == ComparisonMode::Joint) {
        ok = w.intersects(one_full, algebra_stats.get());
      } else if (!w.empty()) {
        ok = true;
        for (std::size_t b = 0; b < eff_groups; ++b) {
          if (!w.block(2U * b, 2U).intersects(one_2x2, algebra_stats.get())) {
            ok = false;
            break;
          }
        }
      }
      if (ok)
        surviving.push_back(t);
    }
    accumulateAlgebraStats(stats.algebra, *algebra_stats);
    return surviving;
  }

  template <class Word>
  std::vector<State> runSingleSidedParityImpl(
      spds::Direction direction, bool /*call_balanced*/, std::size_t eff_groups,
      const spds::StrongComponents &neutral_scc,
      const std::vector<std::uint32_t> &local_of, State start,
      const std::vector<State> &targets,
      const std::vector<CondensedPdsRule> &condensed_rules,
      SingleSidedParityOptions parity, Clock::time_point comp_started,
      Statistics &stats) const {
    using ParityDomain = ParitySemiring<Word>;
    using ParityWeight = typename ParityDomain::Weight;

    AlgebraStatistics algebra_stats;
    ParityDomain side_domain(eff_groups, &algebra_stats);
    spds::PushdownSystem<ParityDomain> pds(side_domain);
    for (std::uint32_t c = 0; c < neutral_scc.count; ++c)
      pds.addControl();

    const ParityWeight id_weight = side_domain.one();
    std::vector<ParityWeight> toggle_weights;
    toggle_weights.reserve(eff_groups);
    for (std::size_t b = 0; b < eff_groups; ++b)
      toggle_weights.push_back(side_domain.toggle(b));
    stats.prepared_weights += eff_groups + 1U;

    for (const auto &cr : condensed_rules) {
      if (cr.tag == 1) {
        pds.addPreserveRule(cr.cu, cr.cv, toggle_weights[cr.param]);
      } else if (cr.tag == 2) {
        pds.addPushRule(cr.cu, cr.cv,
                        symbol(static_cast<unsigned>(cr.param)), id_weight);
      } else if (cr.tag == 3) {
        pds.addRule(cr.cu, symbol(static_cast<unsigned>(cr.param)), cr.cv, {},
                    id_weight);
      } else {
        pds.addPreserveRule(cr.cu, cr.cv, id_weight);
      }
    }
    stats.compiled_rules += pds.rules().size();
    stats.saturation.projection_microseconds += elapsed(comp_started);

    const State cond_start = neutral_scc.component_of[local_of[start]];
    auto seed =
        spds::RegularSet::singleton(pds.controls(), {cond_start, {0}});
    auto aut = direction == spds::Direction::Post
                   ? spds::postStar(pds, seed, options.limits)
                   : spds::preStar(pds, seed, options.limits);
    add(stats.saturation, aut.statistics());
    for (const auto &entry : aut.transitions())
      stats.maximum_affine_rank =
          std::max(stats.maximum_affine_rank, entry.weight.rank());

    const std::size_t batch_threshold =
        options.readout_batch_threshold ? options.readout_batch_threshold : 8;
    std::optional<std::vector<ParityWeight>> batch_weights;
    if (targets.size() >= batch_threshold)
      batch_weights = aut.controlWeights({0});

    const auto read_started = Clock::now();
    std::vector<State> surviving;
    surviving.reserve(targets.size());
    for (State t : targets) {
      const State ct = neutral_scc.component_of[local_of[t]];
      const ParityWeight w =
          batch_weights ? (*batch_weights)[ct] : aut.weight(ct, {0});
      stats.maximum_affine_rank = std::max(stats.maximum_affine_rank, w.rank());
      const bool ok =
          parity.mode == ComparisonMode::Joint
              ? side_domain.containsZero(w)
              : side_domain.independentIntersects(w, id_weight);
      if (ok)
        surviving.push_back(t);
    }
    algebra_stats.intersection_us += elapsed(read_started);
    accumulateAlgebraStats(stats.algebra, algebra_stats);
    return surviving;
  }

  template <class Word>
  std::vector<Vertex> filterSymmetricParity(
      Vertex anchor, spds::Direction direction,
      const std::vector<Vertex> &endpoints, ComparisonMode mode,
      Statistics &stats) const {
    const auto proj_started = Clock::now();
    const State start = controls.at(anchor);
    auto live = reachable({start}, direction);
    std::vector<State> destinations;
    destinations.reserve(endpoints.size());
    for (Vertex v : endpoints)
      destinations.push_back(controls.at(v));
    const auto reverse =
        reachable(destinations, direction == spds::Direction::Post
                                    ? spds::Direction::Pre
                                    : spds::Direction::Post);
    for (State v = 0; v < vertices.size(); ++v)
      live[v] = live[v] && reverse[v];
    live[start] = true;

    using ParityDomain = ParitySemiring<Word>;
    using ParityWeight = typename ParityDomain::Weight;
    AlgebraStatistics algebra_stats;
    ParityDomain parity_domain(pure_parity_bits, &algebra_stats);
    spds::PushdownSystem<ParityDomain> calls(parity_domain);
    spds::PushdownSystem<ParityDomain> fields(parity_domain);

    std::vector<State> indices(vertices.size(), Missing);
    for (State v = 0; v < vertices.size(); ++v) {
      if (live[v]) {
        const State id = calls.addControl();
        fields.addControl();
        indices[v] = id;
      }
    }
    std::vector<std::uint64_t> seen_masks;
    seen_masks.reserve(16);
    for (State u = 0; u < vertices.size(); ++u) {
      if (!live[u])
        continue;
      const State lu = indices[u];
      for (auto id : outgoing[u]) {
        const auto &de = dense_edges[id];
        if (!live[de.to])
          continue;
        const State lv = indices[de.to];
        const auto &edge = edges[id];
        const std::uint64_t raw_mask = edge_parity_masks[id];
        if (seen_masks.empty() ||
            std::find(seen_masks.begin(), seen_masks.end(), raw_mask) ==
                seen_masks.end()) {
          seen_masks.push_back(raw_mask);
        }
        const ParityWeight w = parity_domain.point(static_cast<Word>(raw_mask));
        if (edge.label.kind == LabelKind::CloseParenthesis)
          calls.addRule(lu, symbol(edge.label.id), lv, {}, w);
        else if (edge.label.kind == LabelKind::OpenParenthesis)
          calls.addPushRule(lu, lv, symbol(edge.label.id), w);
        else
          calls.addPreserveRule(lu, lv, w);

        if (edge.label.kind == LabelKind::CloseBracket)
          fields.addRule(lu, symbol(edge.label.id), lv, {}, w);
        else if (edge.label.kind == LabelKind::OpenBracket)
          fields.addPushRule(lu, lv, symbol(edge.label.id), w);
        else
          fields.addPreserveRule(lu, lv, w);
      }
    }
    stats.prepared_weights += seen_masks.size();
    stats.compiled_rules += calls.rules().size() + fields.rules().size();
    stats.saturation.projection_microseconds += elapsed(proj_started);

    const State local_start = indices[start];
    auto seed = spds::RegularSet::singleton(calls.controls(), {local_start, {0}});
    auto call_aut = direction == spds::Direction::Post
                        ? spds::postStar(calls, seed, options.limits)
                        : spds::preStar(calls, seed, options.limits);
    auto field_aut = direction == spds::Direction::Post
                         ? spds::postStar(fields, seed, options.limits)
                         : spds::preStar(fields, seed, options.limits);
    add(stats.saturation, call_aut.statistics());
    add(stats.saturation, field_aut.statistics());
    for (const auto *aut : {&call_aut, &field_aut})
      for (const auto &entry : aut->transitions())
        stats.maximum_affine_rank =
            std::max(stats.maximum_affine_rank, entry.weight.rank());

    std::optional<std::vector<ParityWeight>> call_weights, field_weights;
    if (options.readout_batch_threshold &&
        endpoints.size() >= options.readout_batch_threshold) {
      call_weights =
          options.parentheses == spds::StackAcceptance::Empty
              ? call_aut.controlWeights({0})
              : call_aut.controlWeightsWithPrefix({});
      field_weights =
          options.brackets == spds::StackAcceptance::Empty
              ? field_aut.controlWeights({0})
              : field_aut.controlWeightsWithPrefix({});
    }

    const auto read_started = Clock::now();
    std::vector<Vertex> kept;
    kept.reserve(endpoints.size());
    for (Vertex v : endpoints) {
      const State lv = indices[controls.at(v)];
      if (lv == Missing)
        continue;
      const ParityWeight cw =
          call_weights ? (*call_weights)[lv]
                       : (options.parentheses == spds::StackAcceptance::Empty
                              ? call_aut.weight(lv, {0})
                              : call_aut.weightWithPrefix(lv, {}));
      const ParityWeight fw =
          field_weights ? (*field_weights)[lv]
                        : (options.brackets == spds::StackAcceptance::Empty
                               ? field_aut.weight(lv, {0})
                               : field_aut.weightWithPrefix(lv, {}));
      stats.maximum_affine_rank =
          std::max({stats.maximum_affine_rank, cw.rank(), fw.rank()});
      const bool ok =
          mode == ComparisonMode::Joint
              ? parity_domain.intersects(cw, fw)
              : (mode == ComparisonMode::Independent
                     ? parity_domain.independentIntersects(cw, fw)
                     : (!cw.empty && !fw.empty));
      if (ok)
        kept.push_back(v);
    }
    stats.saturation.readout_microseconds =
        call_aut.statistics().readout_microseconds +
        field_aut.statistics().readout_microseconds;
    algebra_stats.intersection_us += elapsed(read_started);
    accumulateAlgebraStats(stats.algebra, algebra_stats);
    return kept;
  }

  std::vector<Vertex> filterSymmetricParityDispatch(
      Vertex anchor, spds::Direction direction,
      const std::vector<Vertex> &endpoints, ComparisonMode mode,
      Statistics &stats) const {
    if (pure_parity_bits <= 8U)
      return filterSymmetricParity<std::uint8_t>(anchor, direction, endpoints,
                                                 mode, stats);
    if (pure_parity_bits <= 16U)
      return filterSymmetricParity<std::uint16_t>(anchor, direction, endpoints,
                                                  mode, stats);
    if (pure_parity_bits <= 32U)
      return filterSymmetricParity<std::uint32_t>(anchor, direction, endpoints,
                                                  mode, stats);
    return filterSymmetricParity<std::uint64_t>(anchor, direction, endpoints,
                                                mode, stats);
  }

  std::vector<Vertex> filterSingleSided(State start, spds::Direction direction,
                                        const std::vector<Vertex> &surviving_v,
                                        SingleSidedParityOptions parity,
                                        Statistics &stats) const {
    std::vector<State> targets;
    targets.reserve(surviving_v.size());
    for (Vertex v : surviving_v)
      targets.push_back(controls.at(v));
    if (targets.empty() || parity.mode == ComparisonMode::Projection)
      return surviving_v;

    auto live = corridorLive(start, direction, targets);
    const std::size_t brk_obs = countObservedEdges(live, true);
    const std::size_t par_obs = countObservedEdges(live, false);
    const bool first_call = brk_obs <= par_obs;

    const std::size_t before1 = targets.size();
    targets = runSingleSidedSide(start, direction, first_call, live, targets,
                                 parity, stats);
    if (targets.empty())
      return {};
    if (targets.size() < before1)
      live = corridorLive(start, direction, targets);
    targets = runSingleSidedSide(start, direction, !first_call, live, targets,
                                 parity, stats);
    std::vector<Vertex> out;
    out.reserve(targets.size());
    for (State s : targets)
      out.push_back(vertices[s]);
    return out;
  }
};

PreparedAnalysis::PreparedAnalysis(std::shared_ptr<Storage> storage)
    : storage_(std::move(storage)) {}
const HistoryObserver &PreparedAnalysis::observer() const {
  return *storage_->observer;
}
QueryResult
PreparedAnalysis::query(Vertex anchor, spds::Direction direction,
                        const std::vector<Vertex> *endpoints) const {
  if (!storage_->controls.count(anchor))
    throw std::invalid_argument("AffineSPDS query vertex is not in graph");
  const auto start = Clock::now();
  Statistics compilation;
  auto projection =
      storage_->project(anchor, direction, endpoints, compilation);
  const auto projection_us = elapsed(start);
  auto statistics = std::make_shared<AlgebraStatistics>();
  const auto state = projection->controls->at(anchor);
  auto calls = saturate(projection->calls, state, direction,
                        storage_->options.limits, statistics);
  auto fields = saturate(projection->fields, state, direction,
                         storage_->options.limits, statistics);
  QueryResult result(anchor, direction, storage_->options, projection->controls,
                     storage_->observer, std::move(calls), std::move(fields));
  result.statistics_.saturation.projection_microseconds = projection_us;
  result.statistics_.slice_cache_hits = compilation.slice_cache_hits;
  result.statistics_.compiled_rules = compilation.compiled_rules;
  result.statistics_.prepared_weights = compilation.prepared_weights;
  result.statistics_.observer_microseconds = storage_->observer_us;
  return result;
}
QueryResult PreparedAnalysis::queryFrom(Vertex source) const {
  return query(source, spds::Direction::Post);
}
QueryResult PreparedAnalysis::queryTo(Vertex target) const {
  return query(target, spds::Direction::Pre);
}
QueryResult PreparedAnalysis::queryPairImpl(
    Vertex source, Vertex target, const std::vector<unsigned> *calls,
    const std::vector<unsigned> *fields, spds::DemandDirection direction,
    ComparisonMode mode) const {
  if (!storage_->controls.count(source) || !storage_->controls.count(target))
    throw std::invalid_argument("AffineSPDS query vertex is not in graph");
  const spds::Direction dir = direction == spds::DemandDirection::Pre
                                  ? spds::Direction::Pre
                                  : spds::Direction::Post;
  const Vertex anchor = dir == spds::Direction::Post ? source : target;
  const Vertex endpoint = dir == spds::Direction::Post ? target : source;
  const std::vector<Vertex> single_endpoint{endpoint};
  auto filtered = storage_->filterBoolean(anchor, dir, &single_endpoint, calls,
                                          fields);
  if (filtered.surviving.empty() || filtered.corridor_identity ||
      mode == ComparisonMode::Projection) {
    auto local_controls = std::make_shared<std::map<Vertex, State>>();
    local_controls->emplace(anchor, 0);
    if (endpoint != anchor)
      local_controls->emplace(endpoint, 1);
    auto algebra_stats = std::make_shared<AlgebraStatistics>();
    auto empty_calls = emptyAutomaton(storage_->domain, local_controls->size(),
                                      dir, algebra_stats);
    auto empty_fields = emptyAutomaton(storage_->domain, local_controls->size(),
                                       dir, algebra_stats);
    QueryResult result(anchor, dir, storage_->options,
                       std::move(local_controls), storage_->observer,
                       std::move(empty_calls), std::move(empty_fields));
    result.statistics_.saturation = filtered.statistics;
    result.statistics_.observer_microseconds = storage_->observer_us;
    const auto weight = filtered.surviving.empty() ? storage_->domain.zero()
                                                   : storage_->domain.one();
    if (calls || fields) {
      result.stack_cache_.emplace(
          QueryResult::StackKey{endpoint, calls ? *calls : std::vector<unsigned>{},
                                fields ? *fields : std::vector<unsigned>{}},
          HistoryComparison(weight, weight, std::move(algebra_stats)));
    } else {
      result.cache_.emplace(
          endpoint, HistoryComparison(weight, weight, std::move(algebra_stats)));
    }
    return result;
  }
  auto result = query(anchor, dir, &filtered.surviving);
  addTimings(result.statistics_.saturation, filtered.statistics);
  return result;
}
QueryResult PreparedAnalysis::queryPair(Vertex source, Vertex target,
                                        spds::DemandDirection direction,
                                        ComparisonMode mode) const {
  return queryPairImpl(source, target, nullptr, nullptr, direction, mode);
}
QueryResult PreparedAnalysis::queryPairStacks(
    Vertex source, Vertex target, const std::vector<unsigned> &calls,
    const std::vector<unsigned> &fields, spds::DemandDirection direction,
    ComparisonMode mode) const {
  return queryPairImpl(source, target, &calls, &fields, direction, mode);
}
HistoryComparison PreparedAnalysis::compare(
    Vertex source, Vertex target, spds::DemandDirection direction) const {
  auto result = queryPair(source, target, direction);
  const Vertex endpoint =
      result.direction() == spds::Direction::Post ? target : source;
  return result.compare(endpoint);
}

Result PreparedAnalysis::analyzeAnchor(Vertex anchor, spds::Direction direction,
                                       ComparisonMode mode) const {
  if (!storage_->controls.count(anchor))
    throw std::invalid_argument("AffineSPDS query vertex is not in graph");
  Result result;
  result.mode = mode;
  result.statistics.matrix_dimension = observer().dimension();
  result.statistics.coordinate_dimension = storage_->domain.coordinates();
  result.statistics.observer_microseconds = storage_->observer_us;
  result.pairs.insert({anchor, anchor});

  auto filtered = storage_->filterBoolean(anchor, direction, nullptr);
  if (filtered.surviving.empty() || filtered.corridor_identity ||
      mode == ComparisonMode::Projection) {
    for (Vertex endpoint : filtered.surviving)
      result.pairs.insert(direction == spds::Direction::Post
                              ? Pair{anchor, endpoint}
                              : Pair{endpoint, anchor});
    result.statistics.saturation = filtered.statistics;
    return result;
  }

  if (storage_->pure_parity_bits > 0U) {
    Statistics qstats;
    qstats.matrix_dimension = observer().dimension();
    qstats.coordinate_dimension = storage_->domain.coordinates();
    qstats.observer_microseconds = storage_->observer_us;
    qstats.saturation = filtered.statistics;
    auto kept = storage_->filterSymmetricParityDispatch(
        anchor, direction, filtered.surviving, mode, qstats);
    for (Vertex endpoint : kept)
      result.pairs.insert(direction == spds::Direction::Post
                              ? Pair{anchor, endpoint}
                              : Pair{endpoint, anchor});
    result.statistics = qstats;
    return result;
  }

  auto query = this->query(anchor, direction, &filtered.surviving);
  query.prepareReadout();
  for (Vertex endpoint : filtered.surviving)
    if (selected(query.compare(endpoint), observer(), mode))
      result.pairs.insert(direction == spds::Direction::Post
                              ? Pair{anchor, endpoint}
                              : Pair{endpoint, anchor});
  result.statistics = query.statistics();
  addTimings(result.statistics.saturation, filtered.statistics);
  return result;
}

Result PreparedAnalysis::analyzeFrom(Vertex source, ComparisonMode mode) const {
  auto result = analyzeAnchor(source, spds::Direction::Post, mode);
  result.statistics.saturation.projection_microseconds +=
      storage_->preparation_us;
  return result;
}
Result PreparedAnalysis::analyzeTo(Vertex target, ComparisonMode mode) const {
  auto result = analyzeAnchor(target, spds::Direction::Pre, mode);
  result.statistics.saturation.projection_microseconds +=
      storage_->preparation_us;
  return result;
}
Result PreparedAnalysis::analyzeAll(ComparisonMode mode) const {
  Result result;
  result.mode = mode;
  result.statistics.matrix_dimension = observer().dimension();
  result.statistics.coordinate_dimension = storage_->domain.coordinates();
  result.statistics.observer_microseconds = storage_->observer_us;
  result.statistics.saturation.projection_microseconds =
      storage_->preparation_us;
  for (Vertex source : storage_->vertices) {
    auto per = analyzeAnchor(source, spds::Direction::Post, mode);
    result.pairs.insert(per.pairs.begin(), per.pairs.end());
    add(result.statistics, per.statistics);
  }
  return result;
}
Result PreparedAnalysis::analyzeDemands(const std::vector<Pair> &demands,
                                        ComparisonMode mode,
                                        spds::DemandDirection direction) const {
  std::map<Vertex, std::vector<Vertex>> sources, targets;
  PairSet unique;
  for (const auto &pair : demands) {
    if (!storage_->controls.count(pair.source) ||
        !storage_->controls.count(pair.target))
      throw std::invalid_argument("AffineSPDS demand vertex is not in graph");
    if (unique.insert(pair).second) {
      sources[pair.source].push_back(pair.target);
      targets[pair.target].push_back(pair.source);
    }
  }
  Result result;
  result.mode = mode;
  result.statistics.matrix_dimension = observer().dimension();
  result.statistics.coordinate_dimension = storage_->domain.coordinates();
  result.statistics.observer_microseconds = storage_->observer_us;
  result.statistics.saturation.projection_microseconds =
      storage_->preparation_us;
  const bool prefix_mode =
      storage_->options.parentheses != spds::StackAcceptance::Empty ||
      storage_->options.brackets != spds::StackAcceptance::Empty;
  const bool post =
      prefix_mode
          ? (direction != spds::DemandDirection::Pre)
          : (direction == spds::DemandDirection::Post ||
             (direction == spds::DemandDirection::Auto &&
              sources.size() <= targets.size()));
  const spds::Direction dir =
      post ? spds::Direction::Post : spds::Direction::Pre;
  for (const auto &group : post ? sources : targets) {
    std::vector<Vertex> non_self;
    non_self.reserve(group.second.size());
    for (Vertex endpoint : group.second) {
      if (endpoint == group.first)
        result.pairs.insert({group.first, group.first});
      else
        non_self.push_back(endpoint);
    }
    if (non_self.empty())
      continue;
    auto filtered = storage_->filterBoolean(group.first, dir, &non_self);
    if (filtered.surviving.empty() || filtered.corridor_identity ||
        mode == ComparisonMode::Projection) {
      for (Vertex endpoint : filtered.surviving)
        result.pairs.insert(post ? Pair{group.first, endpoint}
                                 : Pair{endpoint, group.first});
      add(result.statistics.saturation, filtered.statistics);
      continue;
    }
    if (storage_->pure_parity_bits > 0U) {
      Statistics qstats;
      qstats.matrix_dimension = observer().dimension();
      qstats.coordinate_dimension = storage_->domain.coordinates();
      qstats.observer_microseconds = storage_->observer_us;
      qstats.saturation = filtered.statistics;
      auto kept = storage_->filterSymmetricParityDispatch(
          group.first, dir, filtered.surviving, mode, qstats);
      for (Vertex endpoint : kept)
        result.pairs.insert(post ? Pair{group.first, endpoint}
                                 : Pair{endpoint, group.first});
      add(result.statistics, qstats);
      continue;
    }

    auto query = this->query(group.first, dir, &filtered.surviving);
    if (storage_->options.readout_batch_threshold &&
        filtered.surviving.size() >= storage_->options.readout_batch_threshold)
      query.prepareReadout();
    for (Vertex endpoint : filtered.surviving)
      if (selected(query.compare(endpoint), observer(), mode))
        result.pairs.insert(post ? Pair{group.first, endpoint}
                                 : Pair{endpoint, group.first});
    auto qstats = query.statistics();
    addTimings(qstats.saturation, filtered.statistics);
    add(result.statistics, qstats);
  }
  return result;
}

void Solver::validate(const Graph &graph,
                      const HistoryObserver &observer) const {
  if (options_.max_matrix_dimension &&
      observer.dimension() > options_.max_matrix_dimension)
    throw spds::ResourceLimit("AffineSPDS matrix-dimension limit exceeded");
  observer.validate(graph);
  for (const auto &edge : graph.edges())
    switch (edge.label.kind) {
    case LabelKind::OpenParenthesis:
    case LabelKind::CloseParenthesis:
    case LabelKind::OpenBracket:
    case LabelKind::CloseBracket:
    case LabelKind::Neutral:
      break;
    default:
      throw std::invalid_argument("invalid AffineSPDS graph label kind");
    }
}
PreparedAnalysis Solver::prepare(const Graph &graph) const {
  auto opts = options_.observer;
  if (options_.max_matrix_dimension &&
      (!opts.max_matrix_dimension ||
       opts.max_matrix_dimension > options_.max_matrix_dimension))
    opts.max_matrix_dimension = options_.max_matrix_dimension;
  const auto start = Clock::now();
  HistoryObserver observer;
  try {
    observer = HistoryObserver::automatic(graph, opts);
  } catch (const std::length_error &e) {
    throw spds::ResourceLimit(e.what());
  }
  const auto observer_us = elapsed(start);
  auto result = prepare(graph, observer);
  result.storage_->observer_us = observer_us;
  return result;
}
PreparedAnalysis Solver::prepare(const Graph &graph,
                                 const HistoryObserver &observer) const {
  const auto start = Clock::now();
  validate(graph, observer);
  auto storage = std::make_shared<PreparedAnalysis::Storage>(
      options_, graph, std::make_shared<const HistoryObserver>(observer));
  storage->preparation_us = elapsed(start);
  return PreparedAnalysis(std::move(storage));
}

Result PreparedAnalysis::analyzeSingleSidedAnchor(
    Vertex anchor, spds::Direction direction,
    SingleSidedParityOptions parity) const {
  if (!storage_->controls.count(anchor))
    throw std::invalid_argument("AffineSPDS query vertex is not in graph");
  Result result;
  result.mode = parity.mode;
  result.statistics.matrix_dimension =
      static_cast<std::size_t>(parity.groups) * 2U;
  result.statistics.coordinate_dimension =
      parity.groups >= 2U && storage_->options.compress_observer
          ? static_cast<std::size_t>(parity.groups) * 4U
          : result.statistics.matrix_dimension *
                result.statistics.matrix_dimension;
  result.pairs.insert({anchor, anchor});

  auto filtered = storage_->filterBoolean(anchor, direction, nullptr);
  add(result.statistics.saturation, filtered.statistics);
  if (filtered.surviving.empty())
    return result;
  const State start = storage_->controls.at(anchor);
  auto kept = storage_->filterSingleSided(start, direction, filtered.surviving,
                                          parity, result.statistics);
  for (Vertex endpoint : kept)
    result.pairs.insert(direction == spds::Direction::Post
                            ? Pair{anchor, endpoint}
                            : Pair{endpoint, anchor});
  return result;
}

namespace {
void validateSingleSided(const Options &options,
                         const SingleSidedParityOptions &parity) {
  if (parity.groups == 0)
    throw std::invalid_argument("single-sided parity groups must be >= 1");
  if (options.parentheses != spds::StackAcceptance::Empty ||
      options.brackets != spds::StackAcceptance::Empty)
    throw std::invalid_argument(
        "single-sided parity requires empty stack acceptance on both stacks");
  const std::size_t dim = static_cast<std::size_t>(parity.groups) * 2U;
  if (options.max_matrix_dimension && dim > options.max_matrix_dimension)
    throw spds::ResourceLimit("AffineSPDS matrix-dimension limit exceeded");
}
} // namespace

Result Solver::analyzeSingleSidedParityAll(
    const Graph &graph, SingleSidedParityOptions parity) const {
  validateSingleSided(options_, parity);
  auto identity = HistoryObserver(1);
  auto prepared = prepare(graph, identity);
  Result result;
  result.mode = parity.mode;
  result.statistics.matrix_dimension =
      static_cast<std::size_t>(parity.groups) * 2U;
  result.statistics.coordinate_dimension =
      parity.groups >= 2U && options_.compress_observer
          ? static_cast<std::size_t>(parity.groups) * 4U
          : result.statistics.matrix_dimension *
                result.statistics.matrix_dimension;
  result.statistics.saturation.projection_microseconds =
      prepared.storage_->preparation_us;
  for (Vertex source : prepared.storage_->vertices) {
    auto per = prepared.analyzeSingleSidedAnchor(source, spds::Direction::Post,
                                                 parity);
    result.pairs.insert(per.pairs.begin(), per.pairs.end());
    add(result.statistics, per.statistics);
  }
  return result;
}

Result Solver::analyzeSingleSidedParityFrom(
    const Graph &graph, Vertex source, SingleSidedParityOptions parity) const {
  validateSingleSided(options_, parity);
  auto prepared = prepare(graph, HistoryObserver(1));
  auto result =
      prepared.analyzeSingleSidedAnchor(source, spds::Direction::Post, parity);
  result.statistics.saturation.projection_microseconds +=
      prepared.storage_->preparation_us;
  return result;
}

Result Solver::analyzeSingleSidedParityTo(
    const Graph &graph, Vertex target, SingleSidedParityOptions parity) const {
  validateSingleSided(options_, parity);
  auto prepared = prepare(graph, HistoryObserver(1));
  auto result =
      prepared.analyzeSingleSidedAnchor(target, spds::Direction::Pre, parity);
  result.statistics.saturation.projection_microseconds +=
      prepared.storage_->preparation_us;
  return result;
}

Result Solver::analyzeSingleSidedParityDemands(
    const Graph &graph, const std::vector<Pair> &demands,
    SingleSidedParityOptions parity, spds::DemandDirection direction) const {
  validateSingleSided(options_, parity);
  auto prepared = prepare(graph, HistoryObserver(1));
  auto &st = *prepared.storage_;
  std::map<Vertex, std::vector<Vertex>> sources, targets;
  PairSet unique;
  std::vector<Pair> non_self_all;
  non_self_all.reserve(demands.size());
  Result result;
  result.mode = parity.mode;
  result.statistics.matrix_dimension =
      static_cast<std::size_t>(parity.groups) * 2U;
  result.statistics.coordinate_dimension =
      parity.groups >= 2U && options_.compress_observer
          ? static_cast<std::size_t>(parity.groups) * 4U
          : result.statistics.matrix_dimension *
                result.statistics.matrix_dimension;
  result.statistics.saturation.projection_microseconds = st.preparation_us;

  for (const auto &pair : demands) {
    if (!st.controls.count(pair.source) || !st.controls.count(pair.target))
      throw std::invalid_argument("AffineSPDS demand vertex is not in graph");
    if (unique.insert(pair).second) {
      if (pair.source == pair.target) {
        result.pairs.insert(pair);
      } else {
        non_self_all.push_back(pair);
        sources[pair.source].push_back(pair.target);
        targets[pair.target].push_back(pair.source);
      }
    }
  }
  if (non_self_all.empty())
    return result;

  if (hasUnboundedLimits(options_.limits)) {
    const PairSet bool_kept = spds::filterInterleavedBooleanDemands(
        graph, non_self_all, /*include_self_pairs=*/false);
    if (bool_kept.empty())
      return result;
    if (parity.mode == ComparisonMode::Projection) {
      result.pairs.insert(bool_kept.begin(), bool_kept.end());
      return result;
    }
    sources.clear();
    targets.clear();
    for (const Pair &p : bool_kept) {
      sources[p.source].push_back(p.target);
      targets[p.target].push_back(p.source);
    }
    const bool post = direction == spds::DemandDirection::Post ||
                      (direction == spds::DemandDirection::Auto &&
                       sources.size() <= targets.size());
    const spds::Direction dir =
        post ? spds::Direction::Post : spds::Direction::Pre;
    for (const auto &group : post ? sources : targets) {
      const State start = st.controls.at(group.first);
      auto kept = st.filterSingleSided(start, dir, group.second, parity,
                                       result.statistics);
      for (Vertex endpoint : kept)
        result.pairs.insert(post ? Pair{group.first, endpoint}
                                 : Pair{endpoint, group.first});
    }
    return result;
  }

  const bool post = direction == spds::DemandDirection::Post ||
                    (direction == spds::DemandDirection::Auto &&
                     sources.size() <= targets.size());
  const spds::Direction dir =
      post ? spds::Direction::Post : spds::Direction::Pre;
  for (const auto &group : post ? sources : targets) {
    auto filtered = st.filterBoolean(group.first, dir, &group.second);
    add(result.statistics.saturation, filtered.statistics);
    if (filtered.surviving.empty())
      continue;
    const State start = st.controls.at(group.first);
    auto kept = st.filterSingleSided(start, dir, filtered.surviving, parity,
                                     result.statistics);
    for (Vertex endpoint : kept)
      result.pairs.insert(post ? Pair{group.first, endpoint}
                               : Pair{endpoint, group.first});
  }
  return result;
}
} // namespace lotus::cfl::interleaved_dyck::affine
