#pragma once

#include "Solvers/EGraph/Explain.h"
#include "Solvers/EGraph/Extract.h"
#include "Solvers/EGraph/Rewrite.h"

namespace lotus::egraph {

namespace detail {

inline size_t runnerSaturatingShiftLeft(size_t value, size_t shift) {
  constexpr size_t digits = std::numeric_limits<size_t>::digits;
  constexpr size_t limit = std::numeric_limits<size_t>::max();
  if (shift >= digits || value > (limit >> shift)) {
    return limit;
  }
  return value << shift;
}

inline size_t runnerSaturatingAdd(size_t lhs, size_t rhs) {
  constexpr size_t limit = std::numeric_limits<size_t>::max();
  return rhs > limit - lhs ? limit : lhs + rhs;
}

} // namespace detail

enum class StopReasonKind {
  None,
  Saturated,
  IterationLimit,
  NodeLimit,
  TimeLimit,
  Other,
};

struct StopReason {
  StopReasonKind kind = StopReasonKind::None;
  size_t limit = 0;
  double time_limit = 0.0;
  std::string other_message;

  static StopReason saturated() {
    StopReason out;
    out.kind = StopReasonKind::Saturated;
    return out;
  }

  static StopReason iterationLimit(size_t value) {
    StopReason out;
    out.kind = StopReasonKind::IterationLimit;
    out.limit = value;
    return out;
  }

  static StopReason nodeLimit(size_t value) {
    StopReason out;
    out.kind = StopReasonKind::NodeLimit;
    out.limit = value;
    return out;
  }

  static StopReason timeLimit(double seconds) {
    StopReason out;
    out.kind = StopReasonKind::TimeLimit;
    out.time_limit = seconds;
    return out;
  }

  static StopReason other(std::string message) {
    StopReason out;
    out.kind = StopReasonKind::Other;
    out.other_message = std::move(message);
    return out;
  }
};

template <typename T> struct RunnerResult {
  std::optional<T> value;
  std::optional<StopReason> stop_reason;

  static RunnerResult success(T v) {
    RunnerResult out;
    out.value = std::move(v);
    return out;
  }

  static RunnerResult stop(StopReason reason) {
    RunnerResult out;
    out.stop_reason = std::move(reason);
    return out;
  }

  explicit operator bool() const { return value.has_value(); }
};

struct RunnerLimits {
  size_t iter_limit = 30;
  size_t node_limit = 10'000;
  Duration time_limit = std::chrono::seconds(5);
  std::optional<Instant> start_time;

  bool timeExceeded() const {
    return start_time && now() - *start_time > time_limit;
  }

  template <typename L, typename A>
  std::optional<StopReason> check(size_t iteration,
                                  const EGraph<L, A> &egraph) const {
    if (start_time) {
      Instant current = now();
      double elapsed =
          std::chrono::duration<double>(current - *start_time).count();
      if (current - *start_time > time_limit) {
        return StopReason::timeLimit(elapsed);
      }
    }
    size_t size = egraph.totalSize();
    if (size > node_limit) {
      return StopReason::nodeLimit(size);
    }
    if (iteration >= iter_limit) {
      return StopReason::iterationLimit(iteration);
    }
    return std::nullopt;
  }
};

template <typename IterData> struct Iteration {
  size_t egraph_nodes = 0;
  size_t egraph_classes = 0;
  std::unordered_map<Symbol, size_t> applied;
  double hook_time = 0.0;
  double search_time = 0.0;
  double apply_time = 0.0;
  double rebuild_time = 0.0;
  double total_time = 0.0;
  IterData data{};
  size_t n_rebuilds = 0;
  std::optional<StopReason> stop_reason;
};

struct Report {
  size_t iterations = 0;
  StopReason stop_reason;
  size_t egraph_nodes = 0;
  size_t egraph_classes = 0;
  size_t memo_size = 0;
  size_t rebuilds = 0;
  double total_time = 0.0;
  double search_time = 0.0;
  double apply_time = 0.0;
  double rebuild_time = 0.0;
};

template <typename L, typename A = NoAnalysis<L>,
          typename IterData = std::monostate>
class Runner;

template <typename L, typename A, typename IterData> struct IterationData {
  static IterData make(const Runner<L, A, IterData> &) { return IterData{}; }
};

template <typename IterData>
inline std::ostream &operator<<(std::ostream &os,
                                const Iteration<IterData> &iteration) {
  os << "Iteration(nodes=" << iteration.egraph_nodes
     << ", classes=" << iteration.egraph_classes << ")";
  return os;
}

inline std::ostream &operator<<(std::ostream &os, const StopReason &reason) {
  switch (reason.kind) {
  case StopReasonKind::None:
    os << "None";
    break;
  case StopReasonKind::Saturated:
    os << "Saturated";
    break;
  case StopReasonKind::IterationLimit:
    os << "IterationLimit(" << reason.limit << ")";
    break;
  case StopReasonKind::NodeLimit:
    os << "NodeLimit(" << reason.limit << ")";
    break;
  case StopReasonKind::TimeLimit:
    os << "TimeLimit(" << reason.time_limit << ")";
    break;
  case StopReasonKind::Other:
    os << "Other(" << reason.other_message << ")";
    break;
  }
  return os;
}

inline std::ostream &operator<<(std::ostream &os, const Report &report) {
  os << "Runner report\n"
     << "=============\n"
     << "  Stop reason: " << report.stop_reason << "\n"
     << "  Iterations: " << report.iterations << "\n"
     << "  Egraph size: " << report.egraph_nodes << " nodes, "
     << report.egraph_classes << " classes, " << report.memo_size << " memo\n"
     << "  Rebuilds: " << report.rebuilds << "\n"
     << "  Total time: " << report.total_time << "\n"
     << "    Search:  ("
     << (report.total_time == 0.0 ? 0.0
                                  : report.search_time / report.total_time)
     << ") " << report.search_time << "\n"
     << "    Apply:   ("
     << (report.total_time == 0.0 ? 0.0 : report.apply_time / report.total_time)
     << ") " << report.apply_time << "\n"
     << "    Rebuild: ("
     << (report.total_time == 0.0 ? 0.0
                                  : report.rebuild_time / report.total_time)
     << ") " << report.rebuild_time;
  return os;
}

template <typename L, typename A> class RewriteScheduler {
public:
  virtual ~RewriteScheduler() = default;

  virtual bool canStop(size_t) { return true; }

  virtual std::vector<SearchMatches<L>>
  searchRewrite(size_t, const EGraph<L, A> &egraph,
                const Rewrite<L, A> &rewrite, const WorkControl *control) {
    return rewrite.searchWithLimit(egraph, std::numeric_limits<size_t>::max(),
                                   control);
  }

  virtual RunnerResult<std::vector<std::vector<SearchMatches<L>>>>
  searchRewrites(size_t iteration, const EGraph<L, A> &egraph,
                 const std::vector<Rewrite<L, A>> &rewrites,
                 const RunnerLimits &limits, const WorkControl *control) {
    std::vector<std::vector<SearchMatches<L>>> out;
    out.reserve(rewrites.size());
    for (const auto &rewrite : rewrites) {
      out.push_back(searchRewrite(iteration, egraph, rewrite, control));
      if (auto stop_reason = limits.check(iteration, egraph)) {
        return RunnerResult<std::vector<std::vector<SearchMatches<L>>>>::stop(
            *stop_reason);
      }
    }
    return RunnerResult<std::vector<std::vector<SearchMatches<L>>>>::success(
        std::move(out));
  }

  virtual size_t applyRewrite(size_t, EGraph<L, A> &egraph,
                              const Rewrite<L, A> &rewrite,
                              std::vector<SearchMatches<L>> matches,
                              const WorkControl *control) {
    constexpr size_t CHUNK_SIZE = 64;
    size_t applied = 0;
    std::vector<SearchMatches<L>> chunk_matches(1);
    chunk_matches.front().substs.reserve(CHUNK_SIZE);
    for (auto &match : matches) {
      for (size_t begin = 0; begin < match.substs.size(); begin += CHUNK_SIZE) {
        size_t end = std::min(begin + CHUNK_SIZE, match.substs.size());
        auto &chunk = chunk_matches.front();
        chunk.eclass = match.eclass;
        chunk.ast = match.ast;
        chunk.substs.clear();
        for (size_t i = begin; i < end; ++i) {
          chunk.substs.push_back(std::move(match.substs[i]));
        }
        applied = detail::runnerSaturatingAdd(
            applied, rewrite.apply(egraph, chunk_matches).size());
        if (control && control->poll()) {
          return applied;
        }
      }
    }
    return applied;
  }
};

template <typename L, typename A>
class SimpleScheduler final : public RewriteScheduler<L, A> {};

template <typename L, typename A>
class BackoffScheduler final : public RewriteScheduler<L, A> {
public:
  BackoffScheduler &withInitialMatchLimit(size_t limit) {
    default_match_limit_ = limit;
    return *this;
  }

  BackoffScheduler &withBanLength(size_t length) {
    default_ban_length_ = length;
    return *this;
  }

  BackoffScheduler &doNotBan(Symbol name) {
    auto &stats = ruleStats(std::move(name));
    stats.match_limit = std::numeric_limits<size_t>::max();
    return *this;
  }

  BackoffScheduler &ruleMatchLimit(Symbol name, size_t limit) {
    ruleStats(std::move(name)).match_limit = limit;
    return *this;
  }

  BackoffScheduler &ruleBanLength(Symbol name, size_t length) {
    ruleStats(std::move(name)).ban_length = length;
    return *this;
  }

  bool canStop(size_t iteration) override {
    bool any_banned = false;
    size_t min_banned_until = std::numeric_limits<size_t>::max();
    for (const auto &[_, stats] : stats_) {
      if (stats.banned_until > iteration) {
        any_banned = true;
        min_banned_until = std::min(min_banned_until, stats.banned_until);
      }
    }
    if (!any_banned) {
      return true;
    }

    size_t delta = min_banned_until - iteration;
    for (auto &[_, stats] : stats_) {
      if (stats.banned_until > iteration) {
        stats.banned_until -= delta;
      }
    }
    return false;
  }

  std::vector<SearchMatches<L>>
  searchRewrite(size_t iteration, const EGraph<L, A> &egraph,
                const Rewrite<L, A> &rewrite,
                const WorkControl *control) override {
    auto &stats = ruleStats(rewrite.name());
    if (iteration < stats.banned_until) {
      return {};
    }

    size_t threshold = detail::runnerSaturatingShiftLeft(stats.match_limit,
                                                         stats.times_banned);
    size_t search_limit = threshold == std::numeric_limits<size_t>::max()
                              ? threshold
                              : threshold + 1;
    auto matches = rewrite.searchWithLimit(egraph, search_limit, control);
    size_t total = 0;
    for (const auto &match : matches) {
      total = detail::runnerSaturatingAdd(total, match.substs.size());
    }
    if (total > threshold) {
      size_t ban_length = detail::runnerSaturatingShiftLeft(stats.ban_length,
                                                            stats.times_banned);
      ++stats.times_banned;
      stats.banned_until = detail::runnerSaturatingAdd(iteration, ban_length);
      return {};
    }

    ++stats.times_applied;
    return matches;
  }

private:
  struct RuleStats {
    size_t times_applied = 0;
    size_t banned_until = 0;
    size_t times_banned = 0;
    size_t match_limit = 1000;
    size_t ban_length = 5;
  };

  RuleStats &ruleStats(Symbol name) {
    auto [it, inserted] = stats_.try_emplace(std::move(name));
    if (inserted) {
      it->second.match_limit = default_match_limit_;
      it->second.ban_length = default_ban_length_;
    }
    return it->second;
  }

  size_t default_match_limit_ = 1000;
  size_t default_ban_length_ = 5;
  std::unordered_map<Symbol, RuleStats> stats_;
};

template <typename L, typename A, typename IterData> class Runner {
public:
  explicit Runner(A analysis = A())
      : egraph(std::move(analysis)),
        scheduler_(std::make_unique<BackoffScheduler<L, A>>()) {}

  Runner &withExpr(const RecExpr<L> &expr) {
    roots.push_back(egraph.addExpr(expr));
    return *this;
  }

  Runner &withEGraph(EGraph<L, A> graph) {
    egraph = std::move(graph);
    return *this;
  }

  Runner &withIterLimit(size_t limit) {
    limits_.iter_limit = limit;
    return *this;
  }

  Runner &withNodeLimit(size_t limit) {
    limits_.node_limit = limit;
    return *this;
  }

  Runner &withTimeLimit(Duration limit) {
    limits_.time_limit = limit;
    return *this;
  }

  template <typename Hook> Runner &withHook(Hook hook) {
    hooks.push_back([hook = std::move(hook)](
                        Runner &runner) mutable -> std::optional<std::string> {
      using Result = std::invoke_result_t<Hook &, Runner &>;
      if constexpr (std::is_same_v<Result, std::optional<std::string>>) {
        return hook(runner);
      } else if constexpr (std::is_same_v<Result, std::optional<Symbol>>) {
        auto result = hook(runner);
        if (!result) {
          return std::nullopt;
        }
        return result->str();
      } else {
        static_assert(std::is_same_v<Result, void>,
                      "Runner hook must return std::optional<std::string> or "
                      "std::optional<Symbol>");
        return std::nullopt;
      }
    });
    return *this;
  }

  Runner &withExplanationsEnabled() {
    egraph = egraph.withExplanationsEnabled();
    return *this;
  }

  Runner &withExplanationsDisabled() {
    egraph = egraph.withExplanationsDisabled();
    return *this;
  }

  Runner &withoutExplanationLengthOptimization() {
    egraph = egraph.withoutExplanationLengthOptimization();
    return *this;
  }

  Runner &withExplanationLengthOptimization() {
    egraph = egraph.withExplanationLengthOptimization();
    return *this;
  }

  Runner &withExplanationValidation(bool enabled = true) {
    validate_explanations_ = enabled;
    return *this;
  }

  Runner &withScheduler(std::unique_ptr<RewriteScheduler<L, A>> scheduler) {
    scheduler_ = std::move(scheduler);
    return *this;
  }

  Runner &withScheduler(SimpleScheduler<L, A> scheduler) {
    scheduler_ = std::make_unique<SimpleScheduler<L, A>>(std::move(scheduler));
    return *this;
  }

  Runner &withScheduler(BackoffScheduler<L, A> scheduler) {
    scheduler_ = std::make_unique<BackoffScheduler<L, A>>(std::move(scheduler));
    return *this;
  }

  Runner &run(const std::vector<Rewrite<L, A>> &rules) {
    checkRules(rules);
    egraph.rebuild();
    limits_.start_time = now();
    stop_reason = {};
    iterations.clear();

    for (size_t i = 0;; ++i) {
      auto iteration = runOne(i, rules);
      auto maybe_stop = iteration.stop_reason;
      iterations.push_back(iteration);
      if (maybe_stop) {
        stop_reason = *maybe_stop;
        break;
      }
      if (auto limit_stop = limits_.check(i + 1, egraph)) {
        stop_reason = *limit_stop;
        iterations.back().stop_reason = *limit_stop;
        break;
      }
    }
    return *this;
  }

  Report report() const {
    Report out;
    out.iterations = iterations.size();
    out.stop_reason = stop_reason;
    out.egraph_nodes = egraph.totalNumberOfNodes();
    out.egraph_classes = egraph.numberOfClasses();
    out.memo_size = egraph.memoSize();
    for (const auto &iteration : iterations) {
      out.rebuilds += iteration.n_rebuilds;
      out.total_time += iteration.total_time;
      out.search_time += iteration.search_time;
      out.apply_time += iteration.apply_time;
      out.rebuild_time += iteration.rebuild_time;
    }
    return out;
  }

  void printReport(std::ostream &os = std::cout) const {
    os << report() << '\n';
  }

  std::optional<Explanation<L>> explainEquivalence(const RecExpr<L> &lhs,
                                                   const RecExpr<L> &rhs) {
    return lotus::egraph::explainEquivalence(egraph, lhs, rhs);
  }

  std::optional<Explanation<L>> explainMatches(const RecExpr<L> &lhs,
                                               const PatternAst<L> &rhs,
                                               const Subst &subst) {
    return lotus::egraph::explainMatches(egraph, lhs, rhs, subst);
  }

  EGraph<L, A> egraph;
  std::vector<Iteration<IterData>> iterations;
  std::vector<Id> roots;
  StopReason stop_reason;
  std::vector<std::function<std::optional<std::string>(Runner &)>> hooks;

private:
  void checkRules(const std::vector<Rewrite<L, A>> &rules) const {
    std::unordered_map<Symbol, size_t> counts;
    for (const auto &rule : rules) {
      ++counts[rule.name()];
    }
    for (const auto &[name, count] : counts) {
      if (count > 1) {
        std::cerr << "WARNING: rule '" << name << "' appears " << count
                  << " times; scheduling and reporting may be affected\n";
      }
    }
  }

  Iteration<IterData> runOne(size_t iteration_index,
                             const std::vector<Rewrite<L, A>> &rules) {
    Iteration<IterData> iteration;
    iteration.egraph_nodes = egraph.totalSize();
    iteration.egraph_classes = egraph.numberOfClasses();
    Instant iteration_start = now();

    std::optional<StopReason> result = limits_.check(iteration_index, egraph);

    Instant hook_start = now();
    auto saved_hooks = std::move(this->hooks);
    if (!result) {
      for (auto &hook : saved_hooks) {
        if (auto stop = hook(*this)) {
          result = StopReason::other(*stop);
          break;
        }
      }
    }
    this->hooks = std::move(saved_hooks);
    iteration.hook_time =
        std::chrono::duration<double>(now() - hook_start).count();

    size_t egraph_nodes_after_hooks = egraph.totalSize();
    size_t egraph_classes_after_hooks = egraph.numberOfClasses();

    std::vector<std::vector<SearchMatches<L>>> matches;
    Instant search_start = now();
    if (!result) {
      WorkControl search_control{[&]() { return limits_.timeExceeded(); },
                                 1024};
      auto search_result = scheduler_->searchRewrites(
          iteration_index, egraph, rules, limits_, &search_control);
      if (search_result) {
        matches = std::move(*search_result.value);
      } else {
        result = search_result.stop_reason;
      }
    }
    iteration.search_time =
        std::chrono::duration<double>(now() - search_start).count();

    Instant apply_start = now();
    if (!result) {
      WorkControl apply_control{[&]() {
                                  return limits_.timeExceeded() ||
                                         egraph.totalSize() >
                                             limits_.node_limit;
                                },
                                1};
      for (size_t i = 0; i < matches.size(); ++i) {
        size_t applied =
            scheduler_->applyRewrite(iteration_index, egraph, rules[i],
                                     std::move(matches[i]), &apply_control);
        if (applied > 0) {
          iteration.applied[rules[i].name()] += applied;
        }
        if (auto stop = limits_.check(iteration_index, egraph)) {
          result = *stop;
          break;
        }
      }
    }
    iteration.apply_time =
        std::chrono::duration<double>(now() - apply_start).count();

    Instant rebuild_start = now();
    iteration.n_rebuilds = egraph.rebuild();
    if (validate_explanations_ && egraph.areExplanationsEnabled() &&
        !checkEachExplain(egraph, rules)) {
      throw std::runtime_error("EGraph explanation consistency check failed");
    }
    iteration.rebuild_time =
        std::chrono::duration<double>(now() - rebuild_start).count();

    bool can_be_saturated =
        iteration.applied.empty() && scheduler_->canStop(iteration_index) &&
        iteration.egraph_nodes == egraph_nodes_after_hooks &&
        iteration.egraph_classes == egraph_classes_after_hooks &&
        iteration.egraph_nodes == egraph.totalSize() &&
        iteration.egraph_classes == egraph.numberOfClasses();
    if (can_be_saturated && !result) {
      result = StopReason::saturated();
    }

    iteration.data = IterationData<L, A, IterData>::make(*this);
    iteration.total_time =
        std::chrono::duration<double>(now() - iteration_start).count();
    iteration.stop_reason = result;
    return iteration;
  }

  RunnerLimits limits_;
  std::unique_ptr<RewriteScheduler<L, A>> scheduler_;
  bool validate_explanations_ = false;
};

} // namespace lotus::egraph
