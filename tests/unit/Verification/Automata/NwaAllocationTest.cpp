#include "AllocationProbe.h"
#include "Verification/Automata/NwaReduction.h"

#include <cstdlib>
#include <cstring>
#include <new>

#include <gtest/gtest.h>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {
thread_local std::size_t hotDepth = 0;
thread_local std::size_t hotAllocations = 0;
thread_local std::size_t hotEntries = 0;
thread_local std::uint64_t phaseMask = 0;
constexpr const char *phases[] = {"finite-membership",
                                  "lazy-finite-search",
                                  "finite-witness-expansion",
                                  "reachable-double-deckers",
                                  "direct-simulation-fixed-point",
                                  "delayed-simulation-fixed-point",
                                  "balanced-summary-saturation",
                                  "summary-prefix-search",
                                  "summary-witness-expansion",
                                  "pruning-backward-reachability",
                                  "pruning-productive-summaries",
                                  "buchi-tarjan",
                                  "buchi-lasso-path",
                                  "buchi-lasso-reconstruction",
                                  "bisimulation-refinement",
                                  "context-direct-simulation",
                                  "hopcroft-refinement",
                                  "context-delayed-simulation",
                                  "accepting-trap-certification",
                                  "rejecting-trap-certification"};

void observe(const char *phase, bool entering, void *) noexcept {
  if (entering) {
    ++hotDepth;
    ++hotEntries;
    for (std::size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i)
      if (std::strcmp(phase, phases[i]) == 0)
        phaseMask |= std::uint64_t(1) << i;
  } else {
    --hotDepth;
  }
}
void countAllocation() noexcept {
  if (hotDepth)
    ++hotAllocations;
}
void *allocate(std::size_t bytes) {
  countAllocation();
  if (void *p = std::malloc(bytes ? bytes : 1))
    return p;
  throw std::bad_alloc();
}
void *allocateAligned(std::size_t bytes, std::size_t alignment) {
  countAllocation();
#if defined(_WIN32)
  if (void *p = _aligned_malloc(bytes ? bytes : 1, alignment))
    return p;
#else
  void *p = nullptr;
  if (posix_memalign(&p, alignment, bytes ? bytes : 1) == 0)
    return p;
#endif
  throw std::bad_alloc();
}
void freeAligned(void *p) noexcept {
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}
} // namespace

namespace lotus::verification::automata::test {
void resetAllocationProbe() noexcept {
  hotAllocations = hotEntries = 0;
  phaseMask = 0;
}
void observeAllocationPhase(const char *phase, bool entering,
                            void *data) noexcept {
  observe(phase, entering, data);
}
std::size_t allocationCount() noexcept { return hotAllocations; }
std::size_t auditedPhaseCount() noexcept { return hotEntries; }
} // namespace lotus::verification::automata::test

// Instrument the executable, including allocations made by library code.
// Counting is enabled only inside the operation's explicitly audited phase.
void *operator new(std::size_t bytes) { return allocate(bytes); }
void *operator new[](std::size_t bytes) { return allocate(bytes); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void *operator new(std::size_t bytes, const std::nothrow_t &) noexcept {
  try {
    return allocate(bytes);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t bytes, const std::nothrow_t &) noexcept {
  try {
    return allocate(bytes);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept {
  std::free(p);
}
void *operator new(std::size_t bytes, std::align_val_t alignment) {
  return allocateAligned(bytes, static_cast<std::size_t>(alignment));
}
void *operator new[](std::size_t bytes, std::align_val_t alignment) {
  return allocateAligned(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void *p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete[](void *p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
  freeAligned(p);
}
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept {
  freeAligned(p);
}
void *operator new(std::size_t bytes, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  try {
    return allocateAligned(bytes, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t bytes, std::align_val_t alignment,
                     const std::nothrow_t &) noexcept {
  try {
    return allocateAligned(bytes, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *p, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  freeAligned(p);
}
void operator delete[](void *p, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  freeAligned(p);
}

using namespace lotus::verification::automata;

namespace {
std::shared_ptr<NestedWordAutomaton> recursivePair() {
  Alphabet alphabet;
  alphabet.intern("a", LetterKind::Call);
  alphabet.intern("b", LetterKind::Return);
  alphabet.intern("x", LetterKind::Internal);
  auto a = std::make_shared<NestedWordAutomaton>(alphabet);
  for (State q = 0; q < 6; ++q)
    a->addState(q == 0, q == 0 || q == 4 || q == 5);
  a->addTransition(0, 0, 2);
  a->addTransition(1, 0, 3);
  a->addTransition(2, 2, 4);
  a->addTransition(3, 2, 5);
  a->addTransition(4, 1, 0, 0);
  a->addTransition(5, 1, 1, 1);
  return a;
}
} // namespace

TEST(NwaAllocation, ProbeDetectsProcessHeapAllocation) {
  hotAllocations = hotEntries = 0;
  observe("positive-control", true, nullptr);
  void *p = ::operator new(64);
  static_cast<volatile char *>(p)[0] = 1;
  ::operator delete(p);
  observe("positive-control", false, nullptr);
  EXPECT_EQ(hotAllocations, 1U);
  EXPECT_EQ(hotDepth, 0U);
}

TEST(NwaAllocation, PushdownSimulationFixedPointsUseOnlyPreparedWorkspace) {
  auto a = recursivePair();
  auto context = std::make_shared<OperationContext>();
  context->maxWorkspaceBytes = 4 * 1024 * 1024;
  context->hotLoopObserver = observe;
  hotAllocations = hotEntries = 0;
  auto direct = directSimulationGame(*a, context);
  EXPECT_TRUE(direct.contains(1, 0));
  EXPECT_GT(hotEntries, 0U);
  EXPECT_EQ(hotAllocations, 0U);
  EXPECT_EQ(hotDepth, 0U);

  hotAllocations = hotEntries = 0;
  auto delayed = delayedSimulationGame(*a, context);
  EXPECT_TRUE(delayed.contains(0, 1));
  EXPECT_GT(hotEntries, 0U);
  EXPECT_EQ(hotAllocations, 0U);
  EXPECT_EQ(hotDepth, 0U);
}

TEST(NwaAllocation, WorkspaceExhaustionDoesNotFallBackToHeap) {
  auto a = recursivePair();
  auto context = std::make_shared<OperationContext>();
  context->maxWorkspaceBytes = 1;
  EXPECT_THROW(directSimulationGame(*a, context), ResourceLimit);
  EXPECT_THROW(delayedSimulationGame(*a, context), ResourceLimit);
}

TEST(NwaAllocation, LazyDifferenceSearchAndMembershipDoNotAllocateOnHeap) {
  Alphabet alphabet;
  alphabet.intern("a", LetterKind::Call);
  alphabet.intern("b", LetterKind::Return);
  alphabet.intern("x", LetterKind::Internal);
  auto a = std::make_shared<NestedWordAutomaton>(alphabet);
  auto b = std::make_shared<NestedWordAutomaton>(alphabet);
  for (auto automaton : {a, b}) {
    for (State q = 0; q < 4; ++q)
      automaton->addState(q == 0, automaton == a && q == 3);
    automaton->addTransition(0, 0, 1);
    automaton->addTransition(1, 0, 1);
    automaton->addTransition(1, 1, 3, 0);
    automaton->addTransition(1, 1, 2, 1);
    automaton->addTransition(2, 1, 2, 1);
    automaton->addTransition(2, 1, 3, 0);
  }
  auto context = std::make_shared<OperationContext>();
  context->maxWorkspaceBytes = 4 * 1024 * 1024;
  context->hotLoopObserver = observe;
  auto product = difference(a, b, {}, context);
  hotAllocations = hotEntries = 0;
  auto result = isEmpty(product, {}, context);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(hotAllocations, 0U);
  EXPECT_GE(hotEntries, 2U); // exploration and witness expansion
  EXPECT_EQ(hotDepth, 0U);
  EXPECT_TRUE(validateRun(*product, *result.run));

  std::vector<Letter> word(64, 0);
  word.insert(word.end(), 64, 1);
  hotAllocations = hotEntries = 0;
  EXPECT_TRUE(accepts(product, word, WordSemantics::Open, context));
  EXPECT_EQ(hotAllocations, 0U);
  EXPECT_EQ(hotEntries, 1U);
  EXPECT_EQ(hotDepth, 0U);

  auto empty = difference(a, a, {}, context);
  hotAllocations = hotEntries = 0;
  EXPECT_TRUE(isEmpty(empty, {}, context).empty());
  EXPECT_EQ(hotAllocations, 0U);
  EXPECT_GT(hotEntries, 0U);
}

TEST(NwaAllocation, AllAuditedCorePhasesAvoidProcessHeapAllocation) {
  auto a = recursivePair();
  a->setFinal(0, false);
  auto context = std::make_shared<OperationContext>();
  context->maxWorkspaceBytes = 8 * 1024 * 1024;
  context->hotLoopObserver = observe;
  hotAllocations = hotEntries = 0;
  phaseMask = 0;

  DelayedDeadEndRemoval prune(a, context);
  prune.stage();
  auto view = prune.view();
  EXPECT_TRUE(accepts(view, {0, 2}, WordSemantics::Open, context));
  auto finite = isEmpty(view, {}, context);
  ASSERT_FALSE(finite.empty());
  auto buchi = buchiIsEmpty(view, context);
  ASSERT_FALSE(buchi.empty());
  EXPECT_TRUE(validateLasso(*view, *buchi.lasso, context));

  auto growing = std::make_shared<NestedWordAutomaton>(a->alphabet());
  growing->addState(true);
  growing->addState(false, true);
  growing->addTransition(0, 0, 1);
  growing->addTransition(1, 0, 0);
  auto growth = buchiIsEmpty(growing, context);
  ASSERT_FALSE(growth.empty());
  EXPECT_TRUE(validateLasso(*growing, *growth.lasso, context));

  auto direct = reduceSimulationGame(*a, AcceptanceSemantics::Finite, context);
  auto delayed = reduceSimulationGame(*a, AcceptanceSemantics::Buchi, context);
  EXPECT_LE(direct->stateCount(), a->stateCount());
  EXPECT_LE(delayed->stateCount(), a->stateCount());
  delayedSimulation(*a, AcceptanceSemantics::Buchi, context);

  auto dfa = std::make_shared<NestedWordAutomaton>(a->alphabet());
  for (State q = 0; q < 4; ++q)
    dfa->addState(q == 0, q == 1 || q == 2);
  dfa->addTransition(0, 2, 1);
  dfa->addTransition(1, 2, 2);
  dfa->addTransition(2, 2, 2);
  EXPECT_EQ(minimizeDfa(*dfa, context)->stateCount(), 2U);
  auto universal = std::make_shared<NestedWordAutomaton>(a->alphabet());
  universal->addState(true, true);
  universal->addTransition(0, 0, 0);
  universal->addTransition(0, 2, 0);
  universal->addTransition(0, 1, 0, 0);
  universal->addTransition(0, 1, 0, Bottom);
  EXPECT_TRUE(difference(universal, universal, {true}, context)
                  ->initialStates()
                  .empty());

  EXPECT_EQ(hotDepth, 0U);
  EXPECT_EQ(hotAllocations, 0U);
  for (std::size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i)
    EXPECT_NE(phaseMask & (std::uint64_t(1) << i), 0U) << phases[i];
}
