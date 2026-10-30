#include "AllocationProbe.h"
#include "Verification/Automata/IncrementalInclusion.h"
#include "Verification/Automata/NwaOperations.h"
#include "Verification/Automata/NwaReduction.h"
#include "Verification/Automata/NwaSearch.h"

#include <cstring>
#include <random>
#include <set>

#include <gtest/gtest.h>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

using namespace lotus::verification::automata;

namespace {
Alphabet alphabet() {
  Alphabet a;
  a.intern("a", LetterKind::Call);
  a.intern("b", LetterKind::Return);
  a.intern("x", LetterKind::Internal);
  return a;
}
// Independent concrete-stack interpreter, deliberately not the determinizer.
bool referenceAccepts(NestedWordAutomaton &a, const std::vector<Letter> &word) {
  using Configuration = std::pair<State, std::vector<State>>;
  std::set<Configuration> current;
  for (State q : a.initialStates())
    current.insert({q, {}});
  for (Letter l : word) {
    std::set<Configuration> next;
    for (const auto &c : current) {
      auto stack = c.second;
      State h = Bottom;
      if (a.alphabet().kind(l) == LetterKind::Call)
        stack.push_back(c.first);
      else if (a.alphabet().kind(l) == LetterKind::Return && !stack.empty()) {
        h = stack.back();
        stack.pop_back();
      }
      for (State t : a.successors(c.first, l, h))
        next.insert({t, stack});
    }
    current = std::move(next);
  }
  for (const auto &c : current)
    if (a.isFinal(c.first))
      return true;
  return false;
}
std::shared_ptr<NestedWordAutomaton> balanced() {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  a->addState(true); // outermost call site
  a->addState();     // nested calls
  a->addState();     // returning
  a->addState(false, true);
  a->addTransition(0, 0, 1);
  a->addTransition(1, 0, 1);
  a->addTransition(1, 1, 2, 1);
  a->addTransition(2, 1, 2, 1);
  a->addTransition(1, 1, 3, 0);
  a->addTransition(2, 1, 3, 0);
  return a;
}
void words(unsigned depth,
           const std::function<void(const std::vector<Letter> &)> &f) {
  std::vector<Letter> word;
  std::function<void(unsigned)> visit = [&](unsigned left) {
    f(word);
    if (!left)
      return;
    for (Letter a = 0; a < 3; ++a) {
      word.push_back(a);
      visit(left - 1);
      word.pop_back();
    }
  };
  visit(depth);
}
} // namespace

TEST(Nwa, BalancedGoldenLanguage) {
  auto a = balanced();
  auto det = std::make_shared<PowersetDeterminizer>(a);
  auto comp = complement(a);
  for (unsigned n = 1; n <= 100; ++n) {
    std::vector<Letter> word(n, 0);
    word.insert(word.end(), n, 1);
    EXPECT_TRUE(accepts(a, word));
    EXPECT_TRUE(accepts(det, word));
    EXPECT_FALSE(accepts(comp, word));
    word.pop_back();
    EXPECT_FALSE(accepts(a, word));
    EXPECT_TRUE(accepts(comp, word));
  }
  auto intersectionA = intersection(a, det);
  auto materialized = reachable(intersectionA).automaton;
  words(6, [&](const auto &w) {
    EXPECT_EQ(referenceAccepts(*a, w), referenceAccepts(*materialized, w));
  });
}

TEST(Nwa, PendingCallsAndReturns) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  a->addState(true, true);
  a->addTransition(0, 0, 0);
  a->addTransition(0, 1, 0, Bottom);
  a->addTransition(0, 1, 0, 0);
  EXPECT_TRUE(accepts(a, {1, 1, 0, 0}));
  EXPECT_FALSE(accepts(a, {1, 1, 0, 0}, WordSemantics::Closed));
  EXPECT_TRUE(accepts(a, {0, 0, 1, 1}, WordSemantics::Closed));
  NestedRun run{{0, 0, 0, 0, 0}, {1, 0, 1, 0}, {}};
  run.computeMatching(a->alphabet());
  EXPECT_EQ(run.matching, (std::vector<std::int64_t>{-1, 2, 1, -1}));
  EXPECT_TRUE(validateRun(*a, run));
  run.matching[1] = -1;
  EXPECT_FALSE(validateRun(*a, run));
}

TEST(Nwa, ExhaustiveRandomDifferentialWithConcreteStacks) {
  std::mt19937 rng(1729);
  for (unsigned example = 0; example < 12; ++example) {
    auto a = std::make_shared<NestedWordAutomaton>(alphabet());
    auto b = std::make_shared<NestedWordAutomaton>(alphabet());
    for (auto automaton : {a, b}) {
      for (unsigned q = 0; q < 3; ++q)
        automaton->addState(q == 0 || rng() % 4 == 0, rng() % 2);
      for (State q = 0; q < 3; ++q)
        for (State t = 0; t < 3; ++t) {
          for (Letter l : {0U, 2U})
            if (rng() % 3 == 0)
              automaton->addTransition(q, l, t);
          for (State h : {Bottom, 0U, 1U, 2U})
            if (rng() % 4 == 0)
              automaton->addTransition(q, 1, t, h);
        }
    }
    auto det = std::make_shared<PowersetDeterminizer>(b);
    auto comp = complement(b);
    auto diff = difference(a, b);
    auto product = intersection(a, b);
    auto materialized = reachable(diff).automaton;
    auto reduced = reduceDirectSimulation(*a);
    DelayedDeadEndRemoval prune(a);
    prune.stage();
    auto pruned = prune.commit();
    std::size_t shortest = std::numeric_limits<std::size_t>::max();
    words(5, [&](const auto &w) {
      bool av = referenceAccepts(*a, w), bv = referenceAccepts(*b, w);
      if (av)
        shortest = std::min(shortest, w.size());
      EXPECT_EQ(accepts(det, w), bv);
      EXPECT_EQ(accepts(comp, w), !bv);
      EXPECT_EQ(accepts(diff, w), av && !bv);
      EXPECT_EQ(accepts(product, w), av && bv);
      EXPECT_EQ(referenceAccepts(*materialized, w), av && !bv);
      EXPECT_EQ(referenceAccepts(*reduced, w), av);
      EXPECT_EQ(referenceAccepts(*pruned, w), av);
    });
    auto search = isEmpty(a);
    if (shortest != std::numeric_limits<std::size_t>::max()) {
      ASSERT_FALSE(search.empty());
      EXPECT_EQ(search.run->word.size(), shortest);
    }
    if (!search.empty())
      EXPECT_TRUE(validateRun(*a, *search.run));
  }
}

TEST(Nwa, SinkAndCertifiedTraps) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  a->addState(true, true);
  for (Letter l : {0U, 2U})
    a->addTransition(0, l, 0);
  a->addTransition(0, 1, 0, Bottom);
  a->addTransition(0, 1, 0, 0);
  auto d = difference(a, a, {true});
  EXPECT_TRUE(d->initialStates().empty());
  auto b = std::make_shared<NestedWordAutomaton>(alphabet());
  b->addState(true, true); // Accepting, but not a trap.
  auto notTrap = difference(a, b, {true});
  EXPECT_TRUE(accepts(notTrap, {0, 1}));
  EXPECT_FALSE(accepts(notTrap, {}));
  auto det = std::make_shared<PowersetDeterminizer>(b);
  State sink = det->successors(det->initialStates()[0], 2)[0];
  EXPECT_TRUE(det->relation(sink).empty());
  for (Letter l = 0; l < 3; ++l)
    EXPECT_EQ(det->successors(sink, l)[0], sink);
}

TEST(Nwa, CancellationAndLimitsAreNotEmptyResults) {
  auto context = std::make_shared<OperationContext>();
  std::atomic<bool> cancel{true};
  context->cancel = &cancel;
  EXPECT_THROW(reachable(balanced(), context), OperationCancelled);
  EXPECT_THROW(PowersetDeterminizer(balanced(), context), OperationCancelled);
  cancel = false;
  context->maxPairs = 1;
  EXPECT_THROW(reachable(balanced(), context), ResourceLimit);
}

TEST(Nwa, ShortestAndWeightedStructuredWitnesses) {
  auto a = balanced();
  auto result = isEmpty(a);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(result.run->word, (std::vector<Letter>{0, 1}));
  EXPECT_EQ(result.cost, 2U);
  EXPECT_TRUE(validateRun(*a, *result.run));
  EXPECT_TRUE(isEmpty(difference(a, a)).empty());
  a->addState(false, true);
  a->addTransition(0, 2, 4);
  SearchOptions options;
  options.strategy = SearchStrategy::LoopPenalizingAStar;
  options.loopHeads = {4};
  options.callPenalty = 0;
  result = isEmpty(a, options);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(result.run->word, (std::vector<Letter>{0, 1}));
  EXPECT_TRUE(validateRun(*a, *result.run));
  result = isEmpty(a);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(result.run->word, (std::vector<Letter>{2}));
}

TEST(Nwa, EmptyUnboundedRecursionTerminates) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  a->addState(true);
  a->addState(false, true); // unreachable
  a->addTransition(0, 0, 0);
  a->addTransition(0, 1, 0, 0);
  EXPECT_TRUE(isEmpty(a).empty());
  EXPECT_TRUE(buchiIsEmpty(a).empty());
  a->setFinal(0, true);
  auto result = buchiIsEmpty(a);
  ASSERT_FALSE(result.empty());
  EXPECT_FALSE(result.lasso->loop.word.empty());
}

TEST(Nwa, BuchiSeesAcceptingStatesInsideBalancedSummaries) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  a->addState(true);
  a->addState(false, true);
  a->addTransition(0, 0, 1);
  a->addTransition(1, 1, 0, 0);
  auto result = buchiIsEmpty(a);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(result.lasso->stackGrowth, 0U);
  EXPECT_EQ(result.lasso->loop.states.front(),
            result.lasso->loop.states.back());
  a->setFinal(1, false);
  EXPECT_TRUE(buchiIsEmpty(a).empty());
}

TEST(Nwa, BuchiGrowingStacksAndPendingReturns) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  a->addState(true, true);
  a->addTransition(0, 0, 0);
  auto result = buchiIsEmpty(a);
  ASSERT_FALSE(result.empty());
  EXPECT_GT(result.lasso->stackGrowth, 0U);
  auto b = std::make_shared<NestedWordAutomaton>(alphabet());
  b->addState(true, true);
  b->addTransition(0, 1, 0, Bottom);
  EXPECT_FALSE(buchiIsEmpty(b).empty());
  auto terminating = balanced();
  EXPECT_TRUE(buchiIsEmpty(terminating).empty());
}

TEST(Nwa, PruningKeepsAllProductiveDerivationsAndCallerContexts) {
  auto a = balanced();
  a->addState();            // 4: alternate, longer balanced branch
  a->addState();            // 5: reachable but cannot finish
  a->addState(false, true); // 6: graph-reachable only through wrong caller
  a->addTransition(1, 2, 4);
  a->addTransition(4, 1, 3, 0);
  a->addTransition(0, 2, 5);
  a->addTransition(5, 1, 6, 4);
  DelayedDeadEndRemoval prune(a);
  EXPECT_THROW(prune.view(), std::logic_error);
  prune.stage();
  EXPECT_TRUE(prune.liveStates()[4]);
  EXPECT_FALSE(prune.liveStates()[5]);
  EXPECT_FALSE(prune.liveStates()[6]);
  auto compacted = prune.commit();
  EXPECT_EQ(compacted->stateCount(), 5U);
  EXPECT_EQ(a->stateCount(), 7U);
  words(6, [&](const auto &w) {
    EXPECT_EQ(referenceAccepts(*a, w), referenceAccepts(*compacted, w));
    EXPECT_EQ(referenceAccepts(*a, w), accepts(prune.view(), w));
  });
}

TEST(Nwa, DirectSimulationMergesEquivalentCalleePaths) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  a->addState(true);
  a->addState();
  a->addState();
  a->addState(false, true);
  a->addTransition(0, 0, 1);
  a->addTransition(0, 0, 2);
  a->addTransition(1, 2, 1);
  a->addTransition(2, 2, 2);
  a->addTransition(1, 1, 3, 0);
  a->addTransition(2, 1, 3, 0);
  auto relation = directSimulation(*a);
  EXPECT_TRUE(relation.contains(1, 2));
  EXPECT_TRUE(relation.contains(2, 1));
  auto reduced = reduceDirectSimulation(*a);
  EXPECT_LT(reduced->stateCount(), a->stateCount());
  words(6, [&](const auto &w) {
    EXPECT_EQ(referenceAccepts(*a, w), referenceAccepts(*reduced, w));
  });
}

TEST(Nwa, HopcroftHandlesPartialDfaAndRejectsNestedInput) {
  Alphabet alphabet;
  auto x = alphabet.intern("x", LetterKind::Internal);
  auto y = alphabet.intern("y", LetterKind::Internal);
  auto a = std::make_shared<NestedWordAutomaton>(alphabet);
  a->addState(true);
  a->addState(false, true);
  a->addState(false, true);
  a->addState();
  a->addTransition(0, x, 1);
  a->addTransition(0, y, 2);
  for (State q : {1U, 2U}) {
    a->addTransition(q, x, q);
    a->addTransition(q, y, 3);
  }
  auto reduced = minimizeDfa(*a);
  EXPECT_EQ(reduced->stateCount(), 3U);
  std::vector<Letter> word;
  for (unsigned bits = 0; bits < 256; ++bits) {
    word.clear();
    for (unsigned i = 0; i < 8; ++i)
      word.push_back((bits >> i) & 1);
    EXPECT_EQ(referenceAccepts(*a, word), referenceAccepts(*reduced, word));
  }
  EXPECT_THROW(minimizeDfa(*balanced()), std::invalid_argument);
  a->addTransition(0, x, 2);
  EXPECT_THROW(minimizeDfa(*a), std::invalid_argument);
}

TEST(Nwa, IncrementalIndependentAndCumulativeInclusion) {
  auto a = balanced();
  auto empty = std::make_shared<NestedWordAutomaton>(alphabet());
  IncrementalInclusionCheck check(a);
  EXPECT_FALSE(check.checkCandidate(empty).empty());
  EXPECT_FALSE(check.add(empty).empty());
  EXPECT_TRUE(check.checkCandidate(a).empty());
  EXPECT_FALSE(check.check().empty());
  EXPECT_TRUE(check.add(a).empty());
  EXPECT_EQ(check.refinementCount(), 2U);
  check.compact();
  EXPECT_EQ(check.remainder()->stateCount(), 0U);
  EXPECT_TRUE(check.check().empty());
}

TEST(Nwa, DelayedSimulationTracksOutstandingBuchiObligations) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  a->addState(true, true); // 0: accepting every step
  a->addState(true);       // 1: accepting on the next step
  a->addState(false, true);
  a->addState(true); // 3: never pays its obligation
  a->addTransition(0, 2, 0);
  a->addTransition(1, 2, 2);
  a->addTransition(2, 2, 1);
  a->addTransition(3, 2, 3);
  auto direct = directSimulation(*a);
  EXPECT_FALSE(direct.contains(0, 1));
  auto delayed = delayedSimulation(*a, AcceptanceSemantics::Buchi);
  EXPECT_TRUE(delayed.contains(0, 1));
  EXPECT_TRUE(delayed.contains(1, 0));
  EXPECT_FALSE(delayed.contains(0, 3));
  auto reduced = reduceDelayedSimulation(*a, AcceptanceSemantics::Buchi);
  EXPECT_LT(reduced->stateCount(), a->stateCount());
  EXPECT_EQ(buchiIsEmpty(a).empty(), buchiIsEmpty(reduced).empty());
  auto finite = reduceDelayedSimulation(*a, AcceptanceSemantics::Finite);
  words(5, [&](const auto &w) {
    EXPECT_EQ(referenceAccepts(*a, w), referenceAccepts(*finite, w));
  });
}

TEST(Nwa, FiftyCegarRoundsWithMutuallyRecursiveFunctions) {
  auto program = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 120; ++q)
    program->addState(q == 0, q == 79);
  for (State q = 0; q < 79; ++q)
    program->addTransition(q, 2, q + 1);
  for (State q = 80; q < 99; ++q)
    program->addTransition(q, 2, q + 1);
  for (State q = 100; q < 119; ++q)
    program->addTransition(q, 2, q + 1);
  for (State q : {10U, 90U, 110U})
    program->addTransition(q, 2, q);
  program->addTransition(20, 0, 80);
  program->addTransition(60, 0, 100);
  program->addTransition(85, 0, 100);
  program->addTransition(105, 0, 80);
  program->addTransition(99, 1, 21, 20);
  program->addTransition(119, 1, 61, 60);
  program->addTransition(119, 1, 86, 85);
  program->addTransition(99, 1, 106, 105);
  auto context = std::make_shared<OperationContext>();
  context->maxStates = 10000;
  context->maxPairs = 2000000;
  context->maxWitnessNodes = 2000000;
  context->deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(100);
  context->hotLoopObserver = test::observeAllocationPhase;
  test::resetAllocationProbe();
  std::vector<std::vector<Letter>> removed;
  std::size_t peakStates = 0, peakTransitions = 0;
  for (unsigned round = 0; round < 50; ++round) {
    SCOPED_TRACE(round);
    auto witness = isEmpty(program, {}, context);
    ASSERT_FALSE(witness.empty());
    ASSERT_TRUE(validateRun(*program, *witness.run));
    removed.push_back(witness.run->word);
    auto interpolant = std::make_shared<NestedWordAutomaton>(alphabet());
    const auto &word = removed.back();
    for (State q = 0; q <= word.size(); ++q)
      interpolant->addState(q == 0, q == word.size());
    std::vector<State> stack;
    for (State q = 0; q < word.size(); ++q) {
      State h = Bottom;
      auto kind = interpolant->alphabet().kind(word[q]);
      if (kind == LetterKind::Call)
        stack.push_back(q);
      else if (kind == LetterKind::Return && !stack.empty()) {
        h = stack.back();
        stack.pop_back();
      }
      interpolant->addTransition(q, word[q], q + 1, h);
    }
    auto diff =
        reachable(difference(program, interpolant, {}, context), context)
            .automaton;
    peakStates = std::max(peakStates, diff->stateCount());
    peakTransitions = std::max(peakTransitions, diff->transitions().size());
    DelayedDeadEndRemoval prune(diff, context);
    prune.stage();
    program = reduceDirectSimulation(*prune.commit(), context);
    for (const auto &trace : removed)
      EXPECT_FALSE(referenceAccepts(*program, trace));
    // Removing distinct finite words needs additional memory states. Bound
    // growth linearly in refinements, rather than assuming a constant-size
    // representation of a language that changes on every iteration.
    EXPECT_LE(program->stateCount(), 240U + 16U * (round + 1));
  }
  EXPECT_LT(peakStates, 6000U);
  EXPECT_LT(peakTransitions, 7200U);
  EXPECT_EQ(test::allocationCount(), 0U);
  EXPECT_GT(test::auditedPhaseCount(), 0U);
  RecordProperty("hot_loop_heap_allocations",
                 std::to_string(test::allocationCount()));
  RecordProperty("audited_hot_phases",
                 std::to_string(test::auditedPhaseCount()));
  RecordProperty("peak_states", static_cast<int>(peakStates));
  RecordProperty("peak_transitions", static_cast<int>(peakTransitions));
#if defined(__APPLE__) || defined(__linux__)
  struct rusage usage{};
  ASSERT_EQ(getrusage(RUSAGE_SELF, &usage), 0);
  std::uint64_t peakBytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#if defined(__linux__)
  peakBytes *= 1024;
#endif
  EXPECT_LT(peakBytes, 1024ULL * 1024 * 1024);
  RecordProperty("peak_rss_bytes", std::to_string(peakBytes));
#endif
}

TEST(Nwa, DeepMutualRecursionWithPendingFrames) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 3; ++q)
    a->addState(q == 0, q == 0);
  for (State q = 0; q < 3; ++q) {
    a->addTransition(q, 0, (q + 1) % 3);
    a->addTransition((q + 1) % 3, 1, q, q);
  }
  auto det = std::make_shared<PowersetDeterminizer>(a);
  auto comp = complement(a);
  for (unsigned pending = 0; pending < 7; ++pending) {
    std::vector<Letter> word(3000, 0);
    word.insert(word.end(), 3000 - pending, 1);
    EXPECT_EQ(referenceAccepts(*a, word), pending % 3 == 0);
    EXPECT_EQ(accepts(det, word), pending % 3 == 0);
    EXPECT_EQ(accepts(comp, word), pending % 3 != 0);
  }
}

TEST(Nwa, BuchiRejectsCyclesThatConsumeUnavailableStackFrames) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  a->addState(true);
  a->addState();
  a->addState(false, true);
  a->addTransition(0, 0, 1);
  a->addTransition(1, 1, 2, 0);
  a->addTransition(2, 2, 1);
  EXPECT_FALSE(isEmpty(a).empty());
  EXPECT_TRUE(buchiIsEmpty(a).empty());
}

TEST(Nwa, IncrementalCounterexampleLiftingPreservesProductStateIds) {
  auto a = balanced();
  IncrementalInclusionCheck inclusion(a);
  auto before = inclusion.check();
  ASSERT_FALSE(before.empty());
  auto b = std::make_shared<NestedWordAutomaton>(alphabet());
  b->addState(true);
  b->addState(false, true);
  b->addTransition(0, 2, 1);
  auto after = inclusion.add(b);
  ASSERT_FALSE(after.empty());
  EXPECT_EQ(before.run->word, after.run->word);
  EXPECT_TRUE(validateRun(*inclusion.remainder(), *after.run));
  EXPECT_LE(inclusion.remainder()->stateCount(), after.run->word.size() + 1);
}

TEST(Nwa, LazySearchDoesNotExploreBeyondShortestWitness) {
  Alphabet alphabet;
  Letter x = alphabet.intern("x", LetterKind::Internal);
  Letter y = alphabet.intern("y", LetterKind::Internal);
  auto a = std::make_shared<NestedWordAutomaton>(alphabet);
  for (State q = 0; q < 10000; ++q)
    a->addState(q == 0, q == 1);
  a->addTransition(0, x, 1);
  a->addTransition(0, y, 2);
  for (State q = 2; q + 1 < 10000; ++q)
    a->addTransition(q, y, q + 1);
  class GuardedProvider final : public SuccessorProvider {
    std::shared_ptr<NestedWordAutomaton> a_;

  public:
    explicit GuardedProvider(std::shared_ptr<NestedWordAutomaton> a)
        : a_(std::move(a)) {}
    std::size_t queries = 0;
    const Alphabet &alphabet() const override { return a_->alphabet(); }
    const std::vector<State> &initialStates() const override {
      return a_->initialStates();
    }
    std::size_t stateCount() const override { return a_->stateCount(); }
    bool isFinal(State q) const override { return a_->isFinal(q); }
    const std::vector<State> &successors(State q, Letter a, State h) override {
      if (q != 0)
        throw std::logic_error("expanded beyond shortest accepting prefix");
      ++queries;
      return a_->successors(q, a, h);
    }
  };
  auto guarded = std::make_shared<GuardedProvider>(a);
  auto result = isEmpty(guarded);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(result.run->word, (std::vector<Letter>{x}));
  EXPECT_EQ(guarded->queries, 2U);
  EXPECT_LT(result.summaryCount, 10U);
}

TEST(Nwa, LazyClosedSearchExploresCalleesButRequiresMatchedFramesAtGoal) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  a->addState(true);
  a->addState(false, true);
  a->addState(false, true);
  a->addTransition(0, 0, 1);
  a->addTransition(1, 1, 2, 0);
  auto open = isEmpty(a);
  ASSERT_FALSE(open.empty());
  EXPECT_EQ(open.run->word.size(), 1U);
  SearchOptions options;
  options.semantics = WordSemantics::Closed;
  auto closed = isEmpty(a, options);
  ASSERT_FALSE(closed.empty());
  EXPECT_EQ(closed.run->word.size(), 2U);
  EXPECT_TRUE(validateRun(*a, *closed.run, WordSemantics::Closed));
}

TEST(Nwa, BuchiLassoCertificatesAreReplayableAndPumpable) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  a->addState(true);
  a->addState(false, true);
  a->addTransition(0, 0, 1);
  a->addTransition(1, 1, 0, 0);
  auto balancedLoop = buchiIsEmpty(a);
  ASSERT_FALSE(balancedLoop.empty());
  EXPECT_TRUE(validateLasso(*a, *balancedLoop.lasso));
  auto wrong = *balancedLoop.lasso;
  ++wrong.stackGrowth;
  EXPECT_FALSE(validateLasso(*a, wrong));
  wrong = *balancedLoop.lasso;
  wrong.loop.matching.assign(wrong.loop.word.size(), -1);
  EXPECT_FALSE(validateLasso(*a, wrong));

  auto growing = std::make_shared<NestedWordAutomaton>(alphabet());
  growing->addState(true, true);
  growing->addTransition(0, 0, 0);
  auto growth = buchiIsEmpty(growing);
  ASSERT_FALSE(growth.empty());
  EXPECT_TRUE(validateLasso(*growing, *growth.lasso));

  auto pending = std::make_shared<NestedWordAutomaton>(alphabet());
  pending->addState(true, true);
  pending->addTransition(0, 1, 0, Bottom);
  auto pendingLoop = buchiIsEmpty(pending);
  ASSERT_FALSE(pendingLoop.empty());
  EXPECT_TRUE(validateLasso(*pending, *pendingLoop.lasso));

  auto consuming = std::make_shared<NestedWordAutomaton>(alphabet());
  consuming->addState(true);
  consuming->addState(false, true);
  consuming->addTransition(0, 0, 1);
  consuming->addTransition(1, 1, 1, 0);
  BuchiLasso invalid{{{0, 1}, {0}, {-1}}, {{1, 1}, {1}, {-1}}, 0};
  EXPECT_FALSE(validateLasso(*consuming, invalid));
}

TEST(Nwa, DirectSimulationGameMatchesDistinctCallerContexts) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 8; ++q)
    a->addState(q == 0, q == 7);
  a->addTransition(0, 0, 1);
  a->addTransition(0, 0, 2);
  a->addTransition(1, 0, 3);
  a->addTransition(2, 0, 4);
  a->addTransition(3, 1, 5, 1);
  a->addTransition(4, 1, 6, 2);
  a->addTransition(5, 1, 7, 0);
  a->addTransition(6, 1, 7, 0);
  auto conservative = directSimulation(*a);
  EXPECT_FALSE(conservative.contains(1, 2));
  auto full = directSimulationGame(*a);
  EXPECT_TRUE(full.contains(1, 2));
  EXPECT_TRUE(full.contains(2, 1));
  for (State q = 0; q < 8; ++q)
    EXPECT_TRUE(full.contains(q, q));
  a->setFinal(5, true);
  full = directSimulationGame(*a);
  EXPECT_FALSE(full.contains(1, 2));
  EXPECT_TRUE(full.contains(2, 1));
}

TEST(Nwa, DirectSimulationGameDoesNotForgetStackAtEqualUpStates) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 5; ++q)
    a->addState(q == 0, q == 3);
  a->addTransition(0, 0, 2);
  a->addTransition(1, 0, 2);
  a->addTransition(2, 1, 3, 0);
  a->addTransition(2, 1, 4, 1);
  auto full = directSimulationGame(*a);
  EXPECT_TRUE(full.contains(2, 2));
  EXPECT_FALSE(full.contains(0, 1));
  EXPECT_TRUE(full.contains(1, 0));
}

TEST(Nwa, DirectSimulationGameRespectsDuplicatorChoice) {
  for (bool goodResponse : {false, true}) {
    auto a = std::make_shared<NestedWordAutomaton>(alphabet());
    for (State q = 0; q < 7; ++q)
      a->addState(q == 0, q == 5);
    a->addTransition(0, 0, 2);
    a->addTransition(1, 0, 3);
    if (goodResponse)
      a->addTransition(1, 0, 4);
    a->addTransition(2, 1, 5, 0);
    a->addTransition(3, 1, 6, 1);
    a->addTransition(4, 1, 5, 1);
    auto relation = directSimulationGame(*a);
    EXPECT_EQ(relation.contains(0, 1), goodResponse);
  }
}

TEST(Nwa, DirectSimulationGameMatchesIndependentFiniteGameTrees) {
  std::mt19937 rng(94183);
  for (unsigned example = 0; example < 40; ++example) {
    auto a = std::make_shared<NestedWordAutomaton>(alphabet());
    for (State q = 0; q < 5; ++q)
      a->addState(q == 0, rng() % 3 == 0);
    // Every control transition increases its state ID. Thus all plays end,
    // and this independent explicit-stack game tree has no depth cutoff.
    for (State p = 0; p < 5; ++p)
      for (State q = p + 1; q < 5; ++q) {
        for (Letter letter : {0U, 2U})
          if (rng() % 3 == 0)
            a->addTransition(p, letter, q);
        for (State h : {Bottom, 0U, 1U, 2U, 3U, 4U})
          if (rng() % 4 == 0)
            a->addTransition(p, 1, q, h);
      }
    using Stack = std::vector<DoubleDecker>;
    std::function<bool(State, State, const Stack &)> reference;
    reference = [&](State p, State q, const Stack &stack) {
      if (a->isFinal(p) && !a->isFinal(q))
        return false;
      for (Letter letter = 0; letter < 3; ++letter) {
        auto next = stack;
        State ph = Bottom, qh = Bottom;
        if (letter == 0)
          next.emplace_back(p, q);
        else if (letter == 1 && !next.empty()) {
          ph = next.back().first;
          qh = next.back().second;
          next.pop_back();
        }
        for (State pt : a->successors(p, letter, ph)) {
          bool response = false;
          for (State qt : a->successors(q, letter, qh))
            response = response || reference(pt, qt, next);
          if (!response)
            return false;
        }
      }
      return true;
    };
    auto full = directSimulationGame(*a);
    for (State p = 0; p < 5; ++p)
      for (State q = 0; q < 5; ++q) {
        SCOPED_TRACE(example);
        SCOPED_TRACE(p);
        SCOPED_TRACE(q);
        EXPECT_EQ(full.contains(p, q), reference(p, q, {}));
      }
  }
}

TEST(Nwa, DirectSimulationGameAgreesWithRecursiveDeterministicTraceInclusion) {
  std::mt19937 rng(7331);
  for (unsigned example = 0; example < 16; ++example) {
    auto a = std::make_shared<NestedWordAutomaton>(alphabet());
    for (State q = 0; q < 4; ++q)
      a->addState(q == 0, true);
    for (State q = 0; q < 4; ++q) {
      // Internal/call cycles are unrestricted: there is no bound on call depth
      // or trace length in this comparison.
      a->addTransition(q, 0, rng() % 4);
      a->addTransition(q, 2, rng() % 4);
      for (State h : {Bottom, 0U, 1U, 2U, 3U})
        if (rng() % 3 != 0)
          a->addTransition(q, 1, rng() % 4, h);
    }
    auto relation = directSimulationGame(*a);
    std::vector<std::shared_ptr<NestedWordAutomaton>> roots;
    for (State start = 0; start < 4; ++start) {
      auto rooted = std::make_shared<NestedWordAutomaton>(alphabet());
      for (State q = 0; q < 4; ++q)
        rooted->addState(q == start, true);
      for (const auto &t : a->transitions())
        rooted->addTransition(t.source, t.letter, t.target, t.hierarchy);
      roots.push_back(rooted);
    }
    // With deterministic transitions and every state accepting, simulation
    // coincides with inclusion of the finite enabled-trace languages.
    for (State p = 0; p < 4; ++p)
      for (State q = 0; q < 4; ++q) {
        SCOPED_TRACE(example);
        SCOPED_TRACE(p);
        SCOPED_TRACE(q);
        EXPECT_EQ(relation.contains(p, q),
                  isEmpty(difference(roots[p], roots[q])).empty());
      }
  }
}

TEST(Nwa, DelayedSimulationGamePaysDebtInsideMatchingCalls) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 6; ++q)
    a->addState(q == 0, q == 0 || q == 4 || q == 5);
  a->addTransition(0, 0, 2);
  a->addTransition(1, 0, 3);
  a->addTransition(2, 2, 4);
  a->addTransition(3, 2, 5);
  a->addTransition(4, 1, 0, 0);
  a->addTransition(5, 1, 1, 1);
  auto context = std::make_shared<OperationContext>();
  context->deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  EXPECT_FALSE(directSimulationGame(*a, context).contains(0, 1));
  EXPECT_FALSE(delayedSimulation(*a, AcceptanceSemantics::Buchi, context)
                   .contains(0, 1));
  EXPECT_TRUE(delayedSimulationGame(*a, context).contains(0, 1));

  // A callee that can recurse forever without paying the initial debt loses,
  // even though it can return and pay on other plays.
  a->addTransition(2, 0, 2);
  a->addTransition(3, 0, 3);
  a->addTransition(4, 1, 2, 2);
  a->addTransition(5, 1, 3, 3);
  EXPECT_FALSE(delayedSimulationGame(*a, context).contains(0, 1));

  // Paying on callee entry also covers infinitely growing stacks.
  a->setFinal(3, true);
  EXPECT_TRUE(delayedSimulationGame(*a, context).contains(0, 1));
}

TEST(Nwa, DelayedSimulationGameAgreesWithFiniteBuchiGames) {
  std::mt19937 rng(12017);
  for (bool calls : {false, true}) {
    for (unsigned example = 0; example < 30; ++example) {
      auto a = std::make_shared<NestedWordAutomaton>(alphabet());
      for (State q = 0; q < 3; ++q)
        a->addState(q == 0, rng() % 2);
      for (State p = 0; p < 3; ++p)
        for (State q = 0; q < 3; ++q) {
          if (rng() % 3 == 0)
            a->addTransition(p, 2, q);
          if (rng() % 3 == 0)
            a->addTransition(p, calls ? 0 : 1, q, Bottom);
        }
      auto context = std::make_shared<OperationContext>();
      context->deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      // Without returns, stacks are never inspected; without calls these are
      // bottom-stack returns. Both cases reduce to an ordinary finite game.
      auto finite = delayedSimulation(*a, AcceptanceSemantics::Buchi, context);
      auto pushdown = delayedSimulationGame(*a, context);
      for (State p = 0; p < 3; ++p)
        for (State q = 0; q < 3; ++q) {
          SCOPED_TRACE(calls);
          SCOPED_TRACE(example);
          SCOPED_TRACE(p);
          SCOPED_TRACE(q);
          EXPECT_EQ(pushdown.contains(p, q), finite.contains(p, q));
        }
    }
  }
}

TEST(Nwa, DelayedSimulationGamePreservesReflexivityWithRecursiveReturns) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 3; ++q)
    a->addState(q == 0, q == 0);
  for (State q = 0; q < 3; ++q) {
    a->addTransition(q, 0, (q + 1) % 3);
    a->addTransition(q, 2, (q + 2) % 3);
    a->addTransition((q + 1) % 3, 1, q, q);
  }
  auto context = std::make_shared<OperationContext>();
  context->deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  auto relation = delayedSimulationGame(*a, context);
  for (State q = 0; q < 3; ++q)
    EXPECT_TRUE(relation.contains(q, q));
}

TEST(Nwa, DelayedSimulationGameChoosesAnAcceptingRecursiveResponse) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 5; ++q)
    a->addState(q == 0, q == 0 || q == 4);
  a->addTransition(0, 0, 2);
  a->addTransition(1, 0, 3);
  a->addTransition(2, 0, 2);
  a->addTransition(3, 0, 3);
  a->addTransition(2, 1, 0, 0);
  a->addTransition(3, 1, 1, 1);
  a->addTransition(2, 1, 2, 2);
  a->addTransition(3, 1, 3, 3);
  auto context = std::make_shared<OperationContext>();
  context->deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  EXPECT_FALSE(delayedSimulationGame(*a, context).contains(0, 1));
  a->addTransition(1, 0, 4);
  a->addTransition(4, 0, 4);
  a->addTransition(4, 1, 1, 1);
  a->addTransition(4, 1, 4, 4);
  EXPECT_TRUE(delayedSimulationGame(*a, context).contains(0, 1));
}

TEST(Nwa, DelayedSimulationGameMatchesDeterministicCoBuchiCounterexamples) {
  std::mt19937 rng(73349);
  constexpr State n = 3;
  constexpr State sink = 4 * n * n;
  for (unsigned example = 0; example < 20; ++example) {
    auto a = std::make_shared<NestedWordAutomaton>(alphabet());
    for (State q = 0; q < n; ++q)
      a->addState(q == 0, rng() % 2);
    for (State q = 0; q < n; ++q) {
      a->addTransition(q, 0, rng() % n);
      a->addTransition(q, 2, rng() % n);
      for (State h : {Bottom, 0U, 1U, 2U})
        if (rng() % 3 != 0)
          a->addTransition(q, 1, rng() % n, h);
    }
    auto context = std::make_shared<OperationContext>();
    context->deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    auto relation = delayedSimulationGame(*a, context);

    // Independent omega-automaton oracle for deterministic responses. It
    // guesses the last debt-free point and then insists that debt remains
    // outstanding forever. A separate absorbing state records an unmatched
    // Spoiler move. There is no bound on stack height or counterexample length.
    for (State p = 0; p < n; ++p)
      for (State q = 0; q < n; ++q) {
        auto bad = std::make_shared<NestedWordAutomaton>(alphabet());
        bool initialDebt = a->isFinal(p) && !a->isFinal(q);
        State initial = 4 * (p * n + q) + 2 * initialDebt;
        for (State state = 0; state < sink; ++state)
          bad->addState(state == initial ||
                            (initialDebt && state == initial + 1),
                        state % 2);
        bad->addState(false, true);
        bad->addTransition(sink, 2, sink);
        for (State state = 0; state < sink; ++state) {
          State left = (state / 4) / n, right = (state / 4) % n;
          bool debt = (state / 2) % 2, committed = state % 2;
          if (committed && !debt)
            continue;
          for (Letter letter = 0; letter < 3; ++letter) {
            for (State i = 0; i < (letter == 1 ? sink + 1 : 1); ++i) {
              State hierarchy = i == 0 ? Bottom : i - 1;
              State leftHierarchy =
                  hierarchy == Bottom ? Bottom : (hierarchy / 4) / n;
              State rightHierarchy =
                  hierarchy == Bottom ? Bottom : (hierarchy / 4) % n;
              const auto &ls = a->successors(left, letter, leftHierarchy);
              const auto &rs = a->successors(right, letter, rightHierarchy);
              if (ls.empty())
                continue;
              if (rs.empty()) {
                bad->addTransition(state, letter, sink, hierarchy);
                continue;
              }
              bool nextDebt =
                  (debt || a->isFinal(ls.front())) && !a->isFinal(rs.front());
              State target = 4 * (ls.front() * n + rs.front()) + 2 * nextDebt;
              if (!committed)
                bad->addTransition(state, letter, target, hierarchy);
              if (nextDebt)
                bad->addTransition(state, letter, target + 1, hierarchy);
            }
          }
        }
        SCOPED_TRACE(example);
        SCOPED_TRACE(p);
        SCOPED_TRACE(q);
        auto counterexample = buchiIsEmpty(bad, context);
        EXPECT_EQ(relation.contains(p, q), counterexample.empty());
        if (!counterexample.empty())
          EXPECT_TRUE(validateLasso(*bad, *counterexample.lasso, context));
      }
  }
}

TEST(Nwa, SimulationGameReductionMergesDuplicatedNestedProcedures) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 8; ++q)
    a->addState(q == 0, q == 7);
  a->addTransition(0, 0, 1);
  a->addTransition(0, 0, 2);
  a->addTransition(1, 0, 3);
  a->addTransition(2, 0, 4);
  a->addTransition(3, 1, 5, 1);
  a->addTransition(4, 1, 6, 2);
  a->addTransition(5, 1, 7, 0);
  a->addTransition(6, 1, 7, 0);
  auto context = std::make_shared<OperationContext>();
  context->deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  auto reduced = reduceSimulationGame(*a, AcceptanceSemantics::Finite, context);
  EXPECT_LE(reduced->stateCount(), 4U);
  EXPECT_TRUE(isEmpty(difference(a, reduced), {}, context).empty());
  EXPECT_TRUE(isEmpty(difference(reduced, a), {}, context).empty());
}

TEST(Nwa, SimulationGameReductionRejectsUnsafeRootedEquivalence) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 5; ++q)
    a->addState(q == 0, q == 2);
  a->addTransition(0, 0, 1);
  a->addTransition(1, 1, 2, 0);
  a->addTransition(3, 1, 4, 0); // unreachable callee with a different suffix
  a->addTransition(4, 2, 2);
  auto context = std::make_shared<OperationContext>();
  context->deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  auto roots = directSimulationGame(*a, context);
  ASSERT_TRUE(roots.contains(1, 3));
  ASSERT_TRUE(roots.contains(3, 1));
  auto reduced = reduceSimulationGame(*a, AcceptanceSemantics::Finite, context);
  EXPECT_TRUE(accepts(reduced, {0, 1}));
  EXPECT_FALSE(accepts(reduced, {0, 1, 2}));
  EXPECT_TRUE(isEmpty(difference(reduced, a), {}, context).empty());
  EXPECT_TRUE(isEmpty(difference(a, reduced), {}, context).empty());
}

TEST(Nwa, SimulationGameReductionDoesNotCreateBuchiCycles) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 5; ++q)
    a->addState(q == 0, q == 0 || q == 3);
  a->addTransition(0, 0, 1);
  a->addTransition(1, 1, 2, 0);
  a->addTransition(2, 2, 3);
  a->addTransition(4, 1, 0, 0);
  auto context = std::make_shared<OperationContext>();
  context->deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  ASSERT_TRUE(buchiIsEmpty(a, context).empty());
  auto reduced = reduceSimulationGame(*a, AcceptanceSemantics::Buchi, context);
  EXPECT_TRUE(buchiIsEmpty(reduced, context).empty());
}

TEST(Nwa, SimulationGameReductionMergesBuchiCallerCopies) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 4; ++q)
    a->addState(q == 0, q == 0 || q == 2);
  a->addTransition(0, 0, 1);
  a->addTransition(2, 0, 3);
  a->addTransition(1, 1, 0, 0);
  a->addTransition(3, 1, 2, 2);
  a->addTransition(0, 2, 2);
  a->addTransition(2, 2, 2);
  auto context = std::make_shared<OperationContext>();
  context->deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  auto reduced = reduceSimulationGame(*a, AcceptanceSemantics::Buchi, context);
  EXPECT_EQ(reduced->stateCount(), 2U);
  auto result = buchiIsEmpty(reduced, context);
  ASSERT_FALSE(result.empty());
  EXPECT_TRUE(validateLasso(*reduced, *result.lasso, context));
  EXPECT_TRUE(isEmpty(difference(a, reduced), {}, context).empty());
  EXPECT_TRUE(isEmpty(difference(reduced, a), {}, context).empty());
}

TEST(Nwa, SimulationGameReductionScalesAcrossSixtyRecursiveProcedures) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  constexpr State procedures = 60;
  for (State q = 0; q < 2 + 2 * procedures; ++q)
    a->addState(q == 0, q == 1);
  for (State i = 0; i < procedures; ++i) {
    State entry = 2 + 2 * i, body = entry + 1;
    a->addTransition(0, 2, entry);
    a->addTransition(entry, 0, body);
    a->addTransition(body, 0, entry);
    a->addTransition(entry, 2, entry);
    a->addTransition(body, 2, body);
    a->addTransition(body, 1, 1, entry);
    a->addTransition(1, 1, body, body);
  }
  for (auto semantics :
       {AcceptanceSemantics::Finite, AcceptanceSemantics::Buchi}) {
    auto context = std::make_shared<OperationContext>();
    context->deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto start = std::chrono::steady_clock::now();
    auto reduced = reduceSimulationGame(*a, semantics, context);
    EXPECT_LE(reduced->stateCount(), 4U);
    EXPECT_TRUE(isEmpty(difference(a, reduced), {}, context).empty());
    EXPECT_TRUE(isEmpty(difference(reduced, a), {}, context).empty());
    auto lasso = buchiIsEmpty(reduced, context);
    ASSERT_FALSE(lasso.empty());
    EXPECT_TRUE(validateLasso(*reduced, *lasso.lasso, context));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    RecordProperty(semantics == AcceptanceSemantics::Finite
                       ? "direct_milliseconds"
                       : "delayed_milliseconds",
                   std::to_string(ms));
  }
}

TEST(Nwa, SparseLettersKeepContextsAndTotalizationDistinct) {
  Alphabet letters;
  for (unsigned i = 0; i < 1000; ++i)
    letters.intern("i" + std::to_string(i), LetterKind::Internal);
  Letter call = letters.intern("call", LetterKind::Call);
  Letter ret = letters.intern("ret", LetterKind::Return);
  auto a = std::make_shared<NestedWordAutomaton>(letters);
  a->addState(true);
  a->addState(false, true);
  a->addTransition(0, 999, 1);
  a->addTransition(0, 999, 0);
  a->addTransition(0, call, 1);
  a->addTransition(1, ret, 1, 0);
  EXPECT_EQ(a->enabledLetters(0, LetterKind::Internal),
            (std::vector<Letter>{999}));
  EXPECT_EQ(a->enabledLetters(0, LetterKind::Call),
            (std::vector<Letter>{call}));
  EXPECT_TRUE(a->enabledLetters(1, LetterKind::Return, Bottom).empty());
  EXPECT_EQ(a->enabledLetters(1, LetterKind::Return, 0),
            (std::vector<Letter>{ret}));
  auto comp = complement(a);
  EXPECT_EQ(
      comp->enabledLetters(comp->initialStates().front(), LetterKind::Internal)
          .size(),
      1000U);
  EXPECT_TRUE(accepts(comp, {998}));
  EXPECT_FALSE(accepts(comp, {999}));
}

TEST(Nwa,
     CancellationInterruptsActiveAlgorithmsWithoutPublishingPartialResults) {
  auto a = std::make_shared<NestedWordAutomaton>(alphabet());
  for (State q = 0; q < 6; ++q)
    a->addState(q == 0, q == 0 || q == 4 || q == 5);
  a->addTransition(0, 0, 2);
  a->addTransition(1, 0, 3);
  a->addTransition(2, 2, 4);
  a->addTransition(3, 2, 5);
  a->addTransition(4, 1, 0, 0);
  a->addTransition(5, 1, 1, 1);
  std::atomic<bool> cancelled{false};
  auto context = std::make_shared<OperationContext>();
  context->cancel = &cancelled;
  struct Request {
    const char *phase;
    std::atomic<bool> *cancel;
  } request{nullptr, &cancelled};
  context->observerData = &request;
  context->hotLoopObserver = [](const char *phase, bool entering,
                                void *data) noexcept {
    auto &r = *static_cast<Request *>(data);
    if (entering && r.phase && std::strcmp(phase, r.phase) == 0)
      r.cancel->store(true, std::memory_order_relaxed);
  };
  auto interrupt = [&](const char *phase, auto operation) {
    SCOPED_TRACE(phase);
    cancelled = false;
    request.phase = phase;
    EXPECT_THROW(operation(), OperationCancelled);
    EXPECT_TRUE(cancelled.load());
    cancelled = false;
    request.phase = nullptr;
  };
  interrupt("reachable-double-deckers", [&] { reachable(a, context); });
  interrupt("finite-membership",
            [&] { accepts(a, {0, 2}, WordSemantics::Open, context); });
  interrupt("direct-simulation-fixed-point",
            [&] { directSimulationGame(*a, context); });
  interrupt("delayed-simulation-fixed-point",
            [&] { delayedSimulationGame(*a, context); });
  interrupt("buchi-tarjan", [&] { buchiIsEmpty(a, context); });
  auto diff = difference(a, a, {}, context);
  interrupt("lazy-finite-search", [&] { isEmpty(diff, {}, context); });

  DelayedDeadEndRemoval prune(a, context);
  prune.stage();
  auto previous = prune.liveStates();
  interrupt("pruning-productive-summaries", [&] { prune.stage(); });
  EXPECT_EQ(prune.liveStates(), previous);
  context->deadline =
      std::chrono::steady_clock::now() - std::chrono::seconds(1);
  EXPECT_THROW(reachable(a, context), OperationCancelled);
}

TEST(Nwa, RejectingTrapCertificationCollapsesNonacceptingCycles) {
  auto b = std::make_shared<NestedWordAutomaton>(alphabet());
  b->addState(true);
  b->addState();
  for (State q = 0; q < 2; ++q)
    for (State target = 0; target < 2; ++target) {
      b->addTransition(q, 0, target);
      b->addTransition(q, 2, target);
      for (State h : {Bottom, 0U, 1U})
        b->addTransition(q, 1, target, h);
    }
  auto det = std::make_shared<PowersetDeterminizer>(b, defaultContext(), true);
  EXPECT_EQ(det->stateCount(), 1U);
  EXPECT_TRUE(det->relationView(det->initialStates().front()).empty());
  for (Letter letter = 0; letter < 3; ++letter)
    EXPECT_EQ(det->successorsView(0, letter).front(), 0U);
  EXPECT_EQ(det->cachedTransitionCount(), 0U);
  auto a = balanced();
  auto optimized = difference(a, b, {true});
  words(5, [&](const auto &word) {
    EXPECT_EQ(accepts(optimized, word), referenceAccepts(*a, word));
  });
}

TEST(Nwa, FiftyGeneralizedCegarRoundsRemainStableWithPruningAlone) {
  Alphabet letters;
  Letter call = letters.intern("call", LetterKind::Call);
  Letter ret = letters.intern("return", LetterKind::Return);
  Letter loop = letters.intern("loop", LetterKind::Internal);
  std::vector<Letter> selectors;
  for (unsigned i = 0; i < 50; ++i)
    selectors.push_back(
        letters.intern("branch" + std::to_string(i), LetterKind::Internal));
  auto program = std::make_shared<NestedWordAutomaton>(letters);
  for (State q = 0; q < 102; ++q)
    program->addState(q == 0, q == 1);
  for (State i = 0; i < 50; ++i) {
    State entry = 2 + 2 * i, body = entry + 1;
    program->addTransition(0, selectors[i], entry);
    program->addTransition(entry, call, body);
    program->addTransition(body, call, entry);
    program->addTransition(body, loop, body);
    program->addTransition(body, ret, 1, entry);
    program->addTransition(1, ret, body, body);
  }
  auto context = std::make_shared<OperationContext>();
  context->deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  context->hotLoopObserver = test::observeAllocationPhase;
  test::resetAllocationProbe();
  std::vector<std::vector<Letter>> removed;
  std::set<Letter> eliminatedBranches;
  std::size_t peakStates = program->stateCount();
  for (unsigned round = 0; round < 50; ++round) {
    SCOPED_TRACE(round);
    auto witness = isEmpty(program, {}, context);
    ASSERT_FALSE(witness.empty());
    ASSERT_FALSE(witness.run->word.empty());
    removed.push_back(witness.run->word);
    Letter selector = witness.run->word.front();
    ASSERT_TRUE(eliminatedBranches.insert(selector).second);

    // Generalize the spurious trace to its branch prefix. The interpolant
    // accepts that prefix followed by any nested suffix, including open words.
    auto interpolant = std::make_shared<NestedWordAutomaton>(letters);
    interpolant->addState(true);
    interpolant->addState(false, true);
    interpolant->addTransition(0, selector, 1);
    for (Letter a = 0; a < letters.size(); ++a) {
      if (letters.kind(a) == LetterKind::Return) {
        for (State h : {Bottom, 0U, 1U})
          interpolant->addTransition(1, a, 1, h);
      } else {
        interpolant->addTransition(1, a, 1);
      }
    }
    auto materialized =
        reachable(difference(program, interpolant, {true}, context), context)
            .automaton;
    peakStates = std::max(peakStates, materialized->stateCount());
    DelayedDeadEndRemoval prune(materialized, context);
    prune.stage();
    program = prune.commit(); // Deliberately no simulation or DFA minimization.
    EXPECT_LE(program->stateCount(), 102U - 2U * (round + 1));
    for (const auto &trace : removed)
      EXPECT_FALSE(referenceAccepts(*program, trace));
  }
  EXPECT_TRUE(isEmpty(program, {}, context).empty());
  EXPECT_EQ(program->stateCount(), 0U);
  EXPECT_LE(peakStates, 102U);
  EXPECT_EQ(test::allocationCount(), 0U);
  RecordProperty("peak_states", std::to_string(peakStates));
  RecordProperty("hot_loop_heap_allocations",
                 std::to_string(test::allocationCount()));
}
