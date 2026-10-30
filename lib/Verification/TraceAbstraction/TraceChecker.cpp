#include "Verification/TraceAbstraction/TraceChecker.h"

#include "Verification/TraceAbstraction/TransitionFormula.h"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace lotus::verification::traceabstraction {
namespace {
unsigned remaining(nwa::Context context) {
  context->check();
  if (context->deadline == std::chrono::steady_clock::time_point::max())
    return std::numeric_limits<unsigned>::max();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                context->deadline - std::chrono::steady_clock::now())
                .count();
  return static_cast<unsigned>(std::max<int64_t>(
      1, std::min<int64_t>(ms, std::numeric_limits<unsigned>::max())));
}
z3::expr replace(z3::expr e, z3::expr_vector from, z3::expr_vector to) {
  return e.substitute(from, to);
}
z3::expr_vector concat(const z3::expr_vector &a, const z3::expr_vector &b) {
  z3::expr_vector result(a.ctx());
  for (unsigned i = 0; i < a.size(); ++i)
    result.push_back(a[i]);
  for (unsigned i = 0; i < b.size(); ++i)
    result.push_back(b[i]);
  return result;
}
} // namespace

struct TraceChecker::Impl {
  ProgramAutomaton program;
  nwa::Context budget;
  TransitionFormulaBuilder formulas;
  std::shared_ptr<z3::context> owner;
  z3::context &ctx;
  const z3::expr_vector &in, &out, &hier;
  z3::expr_vector outer, predicateArgs;
  z3::solver hoare;
  bool arrayVocabulary = false;
  std::map<std::tuple<unsigned, unsigned, nwa::Letter, unsigned>, bool> cache;
  std::map<unsigned, z3::expr> cachedPredicates;

  Impl(const ProgramAutomaton &p, nwa::Context b)
      : program(p), budget(b), formulas(p, b), owner(formulas.context()),
        ctx(*owner), in(formulas.inputs()), out(formulas.outputs()),
        hier(formulas.hierarchicalInputs()), outer(ctx),
        predicateArgs(concat(in, hier)), hoare(ctx) {
    for (unsigned j = 0; j < in.size(); ++j) {
      outer.push_back(ctx.constant(("outer" + std::to_string(j)).c_str(),
                                   in[j].get_sort()));
      arrayVocabulary |= in[j].is_array();
    }
  }
  z3::expr at(const z3::expr &p, const z3::expr_vector &current,
              const z3::expr_vector &entry) {
    // One simultaneous substitution is essential: old(current) must not be
    // accidentally renamed a second time when current and entry overlap.
    return replace(p, predicateArgs, concat(current, entry));
  }
  bool valid(const z3::expr &formula,
             unsigned queryLimit = std::numeric_limits<unsigned>::max()) {
    budget->check();
    if (&formula.ctx() != &ctx)
      return false;
    auto p = formula.simplify();
    if (p.is_true())
      return true;
    if (p.is_false())
      return false;
    if (arrayVocabulary) {
      auto tactic = z3::tactic(ctx, "simplify") &
                    z3::tactic(ctx, "propagate-values") &
                    z3::tactic(ctx, "solve-eqs") & z3::tactic(ctx, "smt");
      auto solver = tactic.mk_solver();
      solver.set("timeout", std::min(remaining(budget), queryLimit));
      solver.add(!p);
      auto status = solver.check();
      budget->check();
      return status == z3::unsat;
    }
    hoare.set("timeout", remaining(budget));
    hoare.push();
    try {
      hoare.add(!p);
      auto status = hoare.check();
      hoare.pop();
      return status == z3::unsat;
    } catch (...) {
      hoare.pop();
      throw;
    }
  }
  bool inductive(const z3::expr &pre, const z3::expr *caller,
                 nwa::Letter letter, const z3::expr &post) {
    if (&pre.ctx() != &ctx || &post.ctx() != &ctx ||
        (caller && &caller->ctx() != &ctx))
      return false;
    auto kind = program.automaton->alphabet().kind(letter);
    if ((kind == nwa::LetterKind::Return) != (caller != nullptr))
      return false;
    cachedPredicates.emplace(pre.id(), pre);
    cachedPredicates.emplace(post.id(), post);
    if (caller)
      cachedPredicates.emplace(caller->id(), *caller);
    auto key =
        std::make_tuple(pre.id(), caller ? caller->id() : 0, letter, post.id());
    auto found = cache.find(key);
    if (found != cache.end())
      return found->second;
    auto premise = pre && formulas.build(letter).relation;
    auto conclusion = at(post, out, kind == nwa::LetterKind::Call ? in : hier);
    if (caller) {
      premise = premise && at(*caller, hier, outer);
      conclusion = at(post, out, outer);
    }
    bool answer =
        valid(z3::implies(premise, conclusion),
              arrayVocabulary ? 25u : std::numeric_limits<unsigned>::max());
    cache.emplace(key, answer);
    return answer;
  }
  z3::check_result solve(z3::solver &solver, const z3::expr &formula) {
    solver.set("timeout", remaining(budget));
    solver.add(formula);
    return solver.check();
  }

  z3::expr project(const z3::expr &p, const z3::expr_vector &variables) {
    auto formula = variables.empty() ? p : z3::exists(variables, p);
    z3::goal goal(ctx);
    goal.add(formula);
    try {
      auto reduced = z3::try_for(z3::tactic(ctx, "qe-light"),
                                 std::min(remaining(budget), 1000u))(goal);
      auto equivalent = ctx.bool_val(false);
      for (unsigned i = 0; i < reduced.size(); ++i)
        equivalent = equivalent || reduced[i].as_expr();
      return equivalent.simplify();
    } catch (const z3::exception &) {
      budget->check();
      return formula.simplify();
    }
  }

  bool
  forwardCallPredicates(const nwa::NestedRun &run,
                        const std::vector<const TransitionFormula *> &steps,
                        unsigned lastCall, std::vector<z3::expr> &forward) {
    // NestedSsaBuilder + forward projection: keep functional array updates
    // until a call boundary. Projecting after every store would ask QE to
    // invert overwritten arrays and unnecessarily retain quantified heaps.
    z3::expr_vector current(ctx), entry(ctx), fresh(ctx);
    for (unsigned j = 0; j < in.size(); ++j) {
      auto v = ctx.constant(("sp_root_" + std::to_string(j)).c_str(),
                            in[j].get_sort());
      auto h = ctx.constant(("sp_entry_" + std::to_string(j)).c_str(),
                            in[j].get_sort());
      current.push_back(v);
      entry.push_back(h);
      fresh.push_back(v);
      fresh.push_back(h);
    }
    std::map<unsigned, z3::expr_vector> callStates, callEntries;
    auto path = ctx.bool_val(true);
    auto vocabulary = concat(concat(in, out), hier);
    for (unsigned k = 0; k <= lastCall; ++k) {
      budget->check();
      auto kind = program.automaton->alphabet().kind(run.word[k]);
      if (kind == nwa::LetterKind::Call) {
        auto predicate = path;
        for (unsigned j = 0; j < in.size(); ++j)
          predicate = predicate && in[j] == current[j] && hier[j] == entry[j];
        forward[k] = project(predicate.simplify(), fresh);
        callStates.emplace(k, current);
        callEntries.emplace(k, entry);
      }
      if (k == lastCall)
        break;
      auto saved = kind == nwa::LetterKind::Return
                       ? callStates.at(run.matching[k])
                       : entry;
      z3::expr_vector next(ctx);
      for (unsigned j = 0; j < in.size(); ++j) {
        if (steps[k]->havoc.count(j)) {
          auto v = ctx.constant(
              ("sp_havoc_" + std::to_string(k) + "_" + std::to_string(j))
                  .c_str(),
              in[j].get_sort());
          next.push_back(v);
          fresh.push_back(v);
        } else
          next.push_back(current[j]);
      }
      auto substitution = concat(concat(current, next), saved);
      for (const auto &write : steps[k]->assignments) {
        auto rhs = replace(write.second, vocabulary, substitution).simplify();
        next.set(write.first, rhs);
      }
      substitution = concat(concat(current, next), saved);
      auto guard =
          replace(steps[k]->guard, vocabulary, substitution).simplify();
      auto defined =
          replace(steps[k]->defined, vocabulary, substitution).simplify();
      if (!valid(z3::implies(path && guard, defined), 1000u))
        return false;
      path = (path && guard).simplify();
      if (kind == nwa::LetterKind::Call)
        entry = current;
      else if (kind == nwa::LetterKind::Return)
        entry = callEntries.at(run.matching[k]);
      current = next;
    }
    return true;
  }

  bool spWpSequence(const nwa::NestedRun &run,
                    const std::vector<const TransitionFormula *> &steps,
                    TraceCheckResult &result, bool preciseCallContext = false) {
    // Ultimate PredicateTransformer / TraceCheckSpWp: forward predicates at
    // calls supply the hierarchy assumptions for backwards return predicates.
    // First try a modular call context (true). As in Ultimate's modular post
    // operator, unnecessary caller facts need not enter the callee proof.
    // Retry with forward predicates only if that abstraction is too weak.
    std::vector<z3::expr> forward(steps.size() + 1, ctx.bool_val(true));
    unsigned lastCall = 0;
    bool hasReturn = false;
    for (unsigned k = 0; k < steps.size(); ++k)
      if (program.automaton->alphabet().kind(run.word[k]) ==
          nwa::LetterKind::Return) {
        hasReturn = true;
        lastCall = std::max(lastCall, unsigned(run.matching[k]));
      }
    bool functional = preciseCallContext && hasReturn &&
                      forwardCallPredicates(run, steps, lastCall, forward);
    for (unsigned k = 0;
         preciseCallContext && !functional && hasReturn && k < lastCall; ++k) {
      budget->check();
      auto kind = program.automaton->alphabet().kind(run.word[k]);
      auto conjunction = forward[k] && steps[k]->relation;
      auto post = ctx.bool_val(false);
      if (kind == nwa::LetterKind::Call) {
        post = project(conjunction, hier);
        post = replace(post, concat(in, out), concat(hier, in));
      } else if (kind == nwa::LetterKind::Return) {
        conjunction =
            conjunction && at(forward.at(run.matching[k]), hier, outer);
        post = project(conjunction, concat(in, hier));
        post = replace(post, concat(out, outer), concat(in, hier));
      } else
        post = replace(project(conjunction, in), out, in);
      forward[k + 1] = post.simplify();
    }
    result.interpolants.assign(steps.size() + 1, ctx.bool_val(false));
    std::vector<z3::expr> weakest(steps.size(), ctx.bool_val(true));
    std::set<unsigned> neededCallPredicates;
    for (size_t k = steps.size(); k-- > 0;) {
      budget->check();
      auto kind = program.automaton->alphabet().kind(run.word[k]);
      auto post = at(result.interpolants[k + 1], out,
                     kind == nwa::LetterKind::Call     ? in
                     : kind == nwa::LetterKind::Return ? outer
                                                       : hier);
      auto hierarchy = ctx.bool_val(true);
      if (kind == nwa::LetterKind::Return) {
        unsigned call = run.matching[k];
        neededCallPredicates.insert(call);
        hierarchy = at(forward.at(call), hier, outer);
      }
      // Use a sufficient defined-execution WP. Invalid/poisoned effects
      // remain overapproximated in T; requiring defined here makes the Hoare
      // obligation sound without quantifying arbitrary replacement heaps.
      z3::expr_vector from(ctx), to(ctx), projected(ctx);
      for (unsigned j = 0; j < in.size(); ++j) {
        auto assignment = steps[k]->assignments.find(j);
        if (!steps[k]->writes.count(j)) {
          from.push_back(out[j]);
          to.push_back(in[j]);
        } else if (assignment != steps[k]->assignments.end()) {
          from.push_back(out[j]);
          to.push_back(assignment->second);
        } else
          projected.push_back(out[j]);
      }
      auto counterexamples =
          replace(steps[k]->guard && hierarchy && !post, from, to).simplify();
      if (kind == nwa::LetterKind::Return)
        projected = concat(projected, outer);
      auto wp = steps[k]->defined && !project(counterexamples, projected);
      auto disabled = !steps[k]->controlGuard;
      // A branch with a false guard has no successors, including when its
      // other operations would be undefined. Output-dependent choice guards
      // (malloc) are not suitable for this shortcut.
      std::set<unsigned> outputIds;
      for (unsigned j = 0; j < out.size(); ++j)
        outputIds.insert(out[j].id());
      bool outputDependent = false;
      std::vector<z3::expr> pending{disabled};
      std::set<unsigned> visited;
      while (!pending.empty()) {
        auto t = pending.back();
        pending.pop_back();
        if (!visited.insert(t.id()).second)
          continue;
        outputDependent |= outputIds.count(t.id());
        if (t.is_app())
          for (unsigned j = 0; j < t.num_args(); ++j)
            pending.push_back(t.arg(j));
      }
      if (!outputDependent)
        wp = disabled || wp;
      weakest[k] = wp;
      if (neededCallPredicates.count(k))
        wp = wp && forward.at(k);
      result.interpolants[k] = wp.simplify();
    }
    if (!valid(result.interpolants.front(),
               preciseCallContext ? std::numeric_limits<unsigned>::max()
                                  : 2000u))
      return false;
    result.interpolants.front() = ctx.bool_val(true);
    // Check the predicate-transformer obligations instead of asking SMT to
    // rediscover array substitution through T. These are sufficient Hoare
    // obligations: pre => WP(T,post), and on return hierPre => the call
    // assumption used by that WP. This also validates our strengthening of WP.
    for (unsigned k = 0; k < steps.size(); ++k) {
      const auto &pre = result.interpolants[k],
                 &post = result.interpolants[k + 1];
      if (!valid(z3::implies(pre, weakest[k])))
        return false;
      unsigned callerId = 0;
      if (program.automaton->alphabet().kind(run.word[k]) ==
          nwa::LetterKind::Return) {
        unsigned call = run.matching[k];
        const auto &caller = result.interpolants.at(call);
        if (!valid(z3::implies(caller, forward.at(call))))
          return false;
        cachedPredicates.emplace(caller.id(), caller);
        callerId = caller.id();
      }
      cachedPredicates.emplace(pre.id(), pre);
      cachedPredicates.emplace(post.id(), post);
      cache[std::make_tuple(pre.id(), callerId, run.word[k], post.id())] = true;
    }
    return true;
  }

  TraceCheckResult check(const nwa::NestedRun &run) {
    TraceCheckResult result;
    result.context = owner;
    std::vector<const TransitionFormula *> steps;
    for (auto letter : run.word)
      steps.push_back(&formulas.build(letter));

    // Nested SSA, following Ultimate's caller-context restoration. A return
    // reads its caller locals from the matching call's SSA state; heap/global
    // outputs remain connected to the callee's final SSA state.
    std::vector<z3::expr_vector> timed;
    for (unsigned k = 0; k <= steps.size(); ++k) {
      z3::expr_vector row(ctx);
      for (unsigned j = 0; j < in.size(); ++j)
        row.push_back(ctx.constant(
            ("t" + std::to_string(k) + "_v" + std::to_string(j)).c_str(),
            in[j].get_sort()));
      timed.push_back(row);
    }
    auto from = concat(concat(in, out), hier);
    auto path = ctx.bool_val(true), definedPath = ctx.bool_val(true);
    for (unsigned k = 0; k < steps.size(); ++k) {
      budget->check();
      bool isReturn = program.automaton->alphabet().kind(run.word[k]) ==
                      nwa::LetterKind::Return;
      if (isReturn && run.matching[k] < 0)
        throw std::invalid_argument("pending return in trace");
      const auto &caller = isReturn ? timed.at(run.matching[k]) : timed.front();
      auto to = concat(concat(timed[k], timed[k + 1]), caller);
      path = path && replace(steps[k]->relation, from, to);
      definedPath = definedPath && replace(steps[k]->defined, from, to);
    }
    z3::solver replay(ctx);
    auto reachability = solve(replay, path);
    if (reachability == z3::unknown) {
      result.reason = replay.reason_unknown();
      return result;
    }
    if (reachability == z3::sat) {
      if (solve(replay, definedPath) != z3::sat) {
        result.reason = "trace requires poison, invalid/uninitialized memory, "
                        "or an inconclusive replay";
        return result;
      }
      auto model = replay.get_model();
      const auto &variables = formulas.variables();
      for (unsigned k = 0; k < timed.size(); ++k) {
        budget->check();
        for (unsigned j = 0; j < variables.size(); ++j) {
          if (!variables[j].value)
            continue;
          auto label = variables[j].name + "#" + std::to_string(j) + "@" +
                       std::to_string(k);
          result.assignments.emplace(label,
                                     model.eval(timed[k][j], true).to_string());
        }
      }
      result.status = TraceStatus::Feasible;
      return result;
    }

    bool memoryTrace = false;
    for (unsigned j = 0; j < in.size(); ++j)
      memoryTrace |= in[j].is_array();
    if (memoryTrace) {
      // Follow Ultimate's SP/WP route for array formulas. In particular, do
      // not enter the old Spacer array proof path that can abort in Z3 4.12.
      if (!spWpSequence(run, steps, result) &&
          !spWpSequence(run, steps, result, true)) {
        result.reason = "could not project a nested SP/WP proof";
        return result;
      }
    } else {
      // P_k(current, entry) describes one activation, with a frozen snapshot of
      // its caller at entry. Matched returns use TWO antecedents, just as
      // Ultimate's return Hoare check uses linear and hierarchical predicates:
      // P_call(H, O) & P_exit(V, H) & T_return(V,H,V') => P_after(V', O).
      // This is a nested Horn proof, not bounded inlining or a flat-register
      // approximation of recursion. Spacer is the only solver-specific adapter.
      z3::fixedpoint fp(ctx);
      z3::params params(ctx);
      params.set("engine", "spacer");
      params.set("timeout", remaining(budget));
      params.set("xform.inline_eager", false);
      params.set("xform.inline_linear", false);
      params.set("xform.slice", false);
      // Use unsat-core generalization. Every returned cover is checked below.
      params.set("spacer.iuc", 0u);
      params.set("spacer.use_array_eq_generalizer", false);
      fp.set(params);
      z3::sort_vector sorts(ctx);
      for (unsigned j = 0; j < predicateArgs.size(); ++j)
        sorts.push_back(predicateArgs[j].get_sort());
      std::vector<z3::func_decl> relations;
      for (unsigned k = 0; k <= steps.size(); ++k) {
        auto p = ctx.function(("P" + std::to_string(k)).c_str(), sorts,
                              ctx.bool_sort());
        fp.register_relation(p);
        relations.push_back(p);
      }
      auto all = concat(concat(in, out), concat(hier, outer));
      auto quantify = [&](const z3::expr &e) {
        return all.empty() ? e : z3::forall(all, e);
      };
      auto fact = quantify(relations.front()(predicateArgs));
      fp.add_rule(fact, ctx.str_symbol("init"));
      for (unsigned k = 0; k < steps.size(); ++k) {
        budget->check();
        auto kind = program.automaton->alphabet().kind(run.word[k]);
        auto premise = relations[k](predicateArgs) && steps[k]->relation;
        auto args = concat(out, kind == nwa::LetterKind::Call ? in : hier);
        if (kind == nwa::LetterKind::Return) {
          premise =
              premise && relations.at(run.matching[k])(concat(hier, outer));
          args = concat(out, outer);
        }
        auto rule = quantify(z3::implies(premise, relations[k + 1](args)));
        fp.add_rule(rule, ctx.str_symbol(("step" + std::to_string(k)).c_str()));
      }
      auto bad = ctx.function("bad", 0, nullptr, ctx.bool_sort());
      fp.register_relation(bad);
      auto error =
          quantify(z3::implies(relations.back()(predicateArgs), bad()));
      fp.add_rule(error, ctx.str_symbol("error"));
      auto query = bad();
      auto answer = fp.query(query);
      budget->check();
      if (answer != z3::unsat) {
        result.reason = answer == z3::unknown
                            ? fp.reason_unknown()
                            : "nested Horn proof disagrees with SSA replay";
        return result;
      }
      for (auto &p : relations) {
        auto cover = fp.get_cover_delta(-1, p);
        auto args = predicateArgs;
        result.interpolants.push_back(cover.substitute(args).simplify());
      }
      result.interpolants.front() = ctx.bool_val(true);
      result.interpolants.back() = ctx.bool_val(false);
      for (unsigned k = 0; k < steps.size(); ++k) {
        auto premise = result.interpolants[k] && steps[k]->relation;
        if (program.automaton->alphabet().kind(run.word[k]) ==
            nwa::LetterKind::Return)
          premise = premise &&
                    at(result.interpolants.at(run.matching[k]), hier, outer);
        if (valid(!premise))
          result.interpolants[k + 1] = ctx.bool_val(false);
      }
    }
    for (unsigned k = 0; k < steps.size(); ++k) {
      const z3::expr *caller = nullptr;
      if (program.automaton->alphabet().kind(run.word[k]) ==
          nwa::LetterKind::Return)
        caller = &result.interpolants.at(run.matching[k]);
      if (!inductive(result.interpolants[k], caller, run.word[k],
                     result.interpolants[k + 1])) {
        result.interpolants.clear();
        result.reason = "predicate generator did not supply a valid nested "
                        "inductive certificate";
        return result;
      }
    }
    result.status = TraceStatus::Infeasible;
    return result;
  }
};

TraceChecker::TraceChecker(const ProgramAutomaton &p, nwa::Context b)
    : impl_(new Impl(p, b)) {}
TraceChecker::~TraceChecker() = default;
TransitionFormulaBuilder &TraceChecker::semantics() { return impl_->formulas; }
TraceCheckResult TraceChecker::check(const nwa::NestedRun &run) {
  try {
    auto nesting = run;
    nesting.computeMatching(impl_->program.automaton->alphabet());
    if (nesting.matching != run.matching ||
        !nwa::accepts(impl_->program.automaton, run.word,
                      nwa::WordSemantics::Open, impl_->budget))
      throw std::invalid_argument(
          "trace is not a run of the program automaton");
    return impl_->check(run);
  } catch (const std::invalid_argument &e) {
    TraceCheckResult r;
    r.reason = e.what();
    return r;
  }
}
bool TraceChecker::isInductive(const z3::expr &pre, nwa::Letter l,
                               const z3::expr &post) {
  try {
    return impl_->inductive(pre, nullptr, l, post);
  } catch (const std::invalid_argument &) {
    return false;
  }
}
bool TraceChecker::isReturnInductive(const z3::expr &pre, const z3::expr &hier,
                                     nwa::Letter l, const z3::expr &post) {
  try {
    return impl_->inductive(pre, &hier, l, post);
  } catch (const std::invalid_argument &) {
    return false;
  }
}
bool TraceChecker::isTrue(const z3::expr &p) { return impl_->valid(p); }
bool TraceChecker::isFalse(const z3::expr &p) { return impl_->valid(!p); }
} // namespace lotus::verification::traceabstraction
