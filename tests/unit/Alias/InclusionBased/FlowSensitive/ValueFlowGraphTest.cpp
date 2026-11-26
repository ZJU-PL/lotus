#include "Alias/InclusionBased/FlowSensitive/ValueFlow/ValueFlowGraph.h"

#include <array>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace lotus::alias::vfg;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void expect(const Solver &s, NodeID node, PointsToSet expected) {
  if (s.pointsTo(node) == expected)
    return;
  std::ostringstream error;
  error << "node " << node << ": expected {";
  for (ObjectID o : expected)
    error << o << ',';
  error << "}, got {";
  for (ObjectID o : s.pointsTo(node))
    error << o << ',';
  error << '}';
  throw std::runtime_error(error.str());
}
NodeID load(Program &p, BlockID block, NodeID pointer) {
  const auto result = p.addNode();
  p.addLoad(block, pointer, result);
  return result;
}
struct Fixture {
  Program p;
  ObjectID a = p.addObject(false, false, "A");
  ObjectID b = p.addObject(false, false, "B");
  ObjectID x = p.addObject(true, true, "x");
  BlockID entry = p.addBlock();
  Fixture() { p.addRoot(entry); }
};
void straightLine() {
  Fixture f;
  auto &p = f.p;
  auto before = load(p, f.entry, p.address(f.x));
  auto first = p.addStore(f.entry, p.address(f.x), p.address(f.a));
  auto middle = load(p, f.entry, p.address(f.x));
  auto second = p.addStore(f.entry, p.address(f.x), p.address(f.b));
  auto after = load(p, f.entry, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, before, {p.unknownObject()});
  expect(s, middle, {f.a});
  expect(s, after, {f.b});
  require(s.isStrongUpdate(first) && s.isStrongUpdate(second), "strong sites");
  require(s.pointedToBy(f.b).count(after), "inverse relation");
  require(!s.mayAlias(middle, after), "disjoint allocations");
  require(s.mayAlias(before, after), "unknown is a wildcard");
  require(!s.statistics().usedWeakFallback, "straight-line fast path");
  auto traversals = s.statistics().objectTraversals;
  s.analyze();
  expect(s, after, {f.b});
  require(traversals == s.statistics().objectTraversals,
          "repeat analyze reset");
}
void diamond() {
  Fixture f;
  auto &p = f.p;
  auto left = p.addBlock(), right = p.addBlock(), join = p.addBlock();
  p.addControlEdge(f.entry, left);
  p.addControlEdge(f.entry, right);
  p.addControlEdge(left, join);
  p.addControlEdge(right, join);
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  p.addStore(left, p.address(f.x), p.address(f.b));
  auto a = load(p, right, p.address(f.x));
  auto both = load(p, join, p.address(f.x));
  p.addStore(join, p.address(f.x), p.address(f.b));
  auto killed = load(p, join, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, a, {f.a});
  expect(s, both, {f.a, f.b});
  expect(s, killed, {f.b});
  require(s.statistics().iteratedDominanceFrontierNodes != 0,
          "diamond stores require an IDF join");
  require(s.statistics().sparseGraphNodes != 0,
          "store-plus-IDF graph was not built");
}
void fullyDefinedDiamondDropsInitializer() {
  Fixture f;
  auto &p = f.p;
  auto left = p.addBlock(), right = p.addBlock(), join = p.addBlock();
  p.addControlEdge(f.entry, left);
  p.addControlEdge(f.entry, right);
  p.addControlEdge(left, join);
  p.addControlEdge(right, join);
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  p.addStore(left, p.address(f.x), p.address(f.b));
  p.addStore(right, p.address(f.x), p.nullValue());
  auto result = load(p, join, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, result, {p.nullObject(), f.b});
  require(!s.pointsTo(result).count(f.a),
          "IDF join retained a definition killed on every branch");
}
void loop() {
  Fixture f;
  auto &p = f.p;
  auto header = p.addBlock(), body = p.addBlock(), exit = p.addBlock();
  p.addControlEdge(f.entry, header);
  p.addControlEdge(header, body);
  p.addControlEdge(header, exit);
  p.addControlEdge(body, header);
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  auto phi = load(p, header, p.address(f.x));
  p.addStore(body, p.address(f.x), p.address(f.b));
  auto bodyValue = load(p, body, p.address(f.x));
  auto result = load(p, exit, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, phi, {f.a, f.b});
  expect(s, bodyValue, {f.b});
  expect(s, result, {f.a, f.b});
}
void weakAliasing() {
  Fixture f;
  auto &p = f.p;
  auto y = p.addObject(true);
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  p.addStore(f.entry, p.address(y), p.address(f.a));
  auto select = p.addNode();
  p.addCopy(p.address(f.x), select);
  p.addCopy(p.address(y), select);
  auto store = p.addStore(f.entry, select, p.address(f.b));
  auto x = load(p, f.entry, p.address(f.x));
  auto yr = load(p, f.entry, p.address(y));
  Solver s(p);
  s.analyze();
  expect(s, x, {f.a, f.b});
  expect(s, yr, {f.a, f.b});
  require(!s.isStrongUpdate(store), "multi-target store must be weak");
}
void nullStoreKills() {
  Fixture f;
  auto &p = f.p;
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  auto store = p.addStore(f.entry, p.address(f.x), p.nullValue());
  auto result = load(p, f.entry, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, result, {p.nullObject()});
  require(s.isStrongUpdate(store), "null stores still kill");
  require(!s.mayAlias(result, p.address(f.a)), "null is not unknown");
}
void escapeOrder() {
  Fixture f;
  auto &p = f.p;
  // x has a lower object ID than holder. Processing objects by numeric ID
  // would incorrectly connect the first store of A to the final load of x.
  auto holder = p.addObject(true, true, "holder");
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  p.addStore(f.entry, p.address(holder), p.address(f.x));
  auto alias = load(p, f.entry, p.address(holder));
  p.addStore(f.entry, alias, p.address(f.b));
  auto result = load(p, f.entry, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, alias, {f.x});
  expect(s, result, {f.b});
  require(!s.statistics().usedWeakFallback, "acyclic escape order");
}
void deepEscapeOrder() {
  Fixture f;
  auto &p = f.p;
  auto one = p.addObject(true), two = p.addObject(true);
  p.setInitializer(two, p.address(one));
  p.setInitializer(one, p.address(f.x));
  auto q = load(p, f.entry, p.address(two));
  auto r = load(p, f.entry, q);
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  p.addStore(f.entry, r, p.address(f.b));
  auto result = load(p, f.entry, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, result, {f.b});
  require(!s.statistics().usedWeakFallback, "global initializer escape order");
}
void scalarCycle() {
  Fixture f;
  auto &p = f.p;
  auto y = p.addObject(true);
  p.setInitializer(f.x, p.address(y));
  p.setInitializer(y, p.address(f.x));
  auto first = load(p, f.entry, p.address(f.x));
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  auto after = load(p, f.entry, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, first, {y});
  expect(s, after, {y, f.a});
  require(s.statistics().usedWeakFallback, "scalar cycle must use fallback");
  require(s.statistics().strongUpdateSites == 0, "fallback disables kills");
}
void heapCycle() {
  Fixture f;
  auto &p = f.p;
  auto heap = p.addObject(false);
  p.setInitializer(heap, p.address(heap));
  auto before = load(p, f.entry, p.address(heap));
  p.addStore(f.entry, p.address(heap), p.address(f.a));
  auto after = load(p, f.entry, p.address(heap));
  Solver s(p);
  s.analyze();
  expect(s, before, {heap});
  expect(s, after, {heap, f.a});
  require(!s.statistics().usedWeakFallback, "non-scalar cycle can be broken");
}
void unknownClobber() {
  Fixture f;
  auto &p = f.p;
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  p.addStore(f.entry, p.unknownValue(), p.address(f.b), false);
  auto after = load(p, f.entry, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, after, {f.a, f.b});
}
void explicitWeakStore() {
  Fixture f;
  auto &p = f.p;
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  auto partial = p.addStore(f.entry, p.address(f.x), p.unknownValue(), false);
  auto after = load(p, f.entry, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, after, {f.a, p.unknownObject()});
  require(!s.isStrongUpdate(partial), "partial writes cannot kill a pointer");
}
void unreachable() {
  Fixture f;
  auto &p = f.p;
  auto dead = p.addBlock();
  p.addControlEdge(dead, f.entry);
  p.addStore(dead, p.address(f.x), p.address(f.b));
  p.setInitializer(f.x, p.address(f.a));
  auto result = load(p, f.entry, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, result, {f.a});
}
void emptyCycle() {
  Fixture f;
  auto &p = f.p;
  auto empty = p.addBlock(), exit = p.addBlock();
  p.addControlEdge(f.entry, empty);
  p.addControlEdge(empty, empty);
  p.addControlEdge(empty, exit);
  p.setInitializer(f.x, p.address(f.a));
  auto result = load(p, exit, p.address(f.x));
  Solver s(p);
  s.analyze();
  expect(s, result, {f.a});
}
void swapAcrossCall() {
  Fixture f;
  auto &p = f.p;
  auto y = p.addObject(true), callee = p.addBlock(), after = p.addBlock();
  p.addControlEdge(f.entry, callee);
  p.addControlEdge(callee, after);
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  p.addStore(f.entry, p.address(y), p.address(f.b));
  auto formalP = p.addNode(), formalQ = p.addNode();
  p.addCopy(p.address(f.x), formalP);
  p.addCopy(p.address(y), formalQ);
  auto oldP = load(p, callee, formalP), oldQ = load(p, callee, formalQ);
  p.addStore(callee, formalP, oldQ);
  p.addStore(callee, formalQ, oldP);
  auto x = load(p, after, p.address(f.x));
  auto yr = load(p, after, p.address(y));
  Solver s(p);
  s.analyze();
  expect(s, x, {f.b});
  expect(s, yr, {f.a});
}
void strongBecomesWeak() {
  // x initially appears to be the sole target of alias; a heap self-cycle
  // later contributes y. Reaching definitions for x must be recomputed.
  Fixture f;
  auto &p = f.p;
  auto y = p.addObject(true), heap = p.addObject(false);
  p.setInitializer(heap, p.address(heap));
  p.addStore(f.entry, p.address(heap), p.address(y));
  auto alias = load(p, f.entry, p.address(heap));
  p.addCopy(p.address(f.x), alias);
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  p.addStore(f.entry, alias, p.address(f.b));
  auto result = load(p, f.entry, p.address(f.x));
  Solver s(p);
  s.analyze();
  require(s.pointsTo(result).count(f.a) && s.pointsTo(result).count(f.b),
          "a revoked singleton must restore reaching definitions");
}
void queryValidation() {
  Fixture f;
  Solver s(f.p);
  bool caught = false;
  try {
    (void)s.pointsTo(f.p.address(f.a));
  } catch (const std::logic_error &) {
    caught = true;
  }
  require(caught, "query before analyze");
  caught = false;
  try {
    f.p.addCopy(f.p.address(f.a), f.p.address(f.b));
  } catch (const std::invalid_argument &) {
    caught = true;
  }
  require(caught, "address source invariant");
  s.analyze();
  caught = false;
  try {
    (void)s.pointsTo(0);
  } catch (const std::out_of_range &) {
    caught = true;
  }
  require(caught, "invalid node ID");
}

// Independent dense memory-state reference. The generated programs have
// complete, constant access-target sets, so this oracle does not share the
// implementation's escape scheduling or projected reaching-definition code.
void differential(unsigned seed, bool strong) {
  std::mt19937 rng(seed);
  Program p;
  std::vector<ObjectID> cells, values;
  for (unsigned i = 0; i < 3; ++i)
    cells.push_back(p.addObject(true));
  for (unsigned i = 0; i < 3; ++i)
    values.push_back(p.addObject(false, false));
  std::vector<BlockID> blocks;
  for (unsigned i = 0; i < 6; ++i)
    blocks.push_back(p.addBlock());
  p.addRoot(blocks.front());
  // All blocks are live. Additional edges include backedges and diamonds.
  for (unsigned i = 1; i < blocks.size(); ++i)
    p.addControlEdge(blocks[i - 1], blocks[i]);
  for (unsigned i = 0; i < 8; ++i)
    p.addControlEdge(blocks[rng() % blocks.size()],
                     blocks[rng() % blocks.size()]);
  std::map<AccessID, PointsToSet> targets, stored;
  std::vector<NodeID> loads;
  for (auto block : blocks) {
    for (unsigned i = 0; i < 5; ++i) {
      PointsToSet target{cells[rng() % cells.size()]};
      if (rng() % 3 == 0)
        target.insert(cells[rng() % cells.size()]);
      auto pointer = p.addNode();
      for (auto object : target)
        p.addCopy(p.address(object), pointer);
      AccessID id;
      if (rng() % 2) {
        auto value = rng() % 4 ? values[rng() % values.size()] : p.nullObject();
        id = p.addStore(block, pointer, p.address(value));
        stored[id] = {value};
      } else {
        auto value = p.addNode();
        id = p.addLoad(block, pointer, value);
        loads.push_back(value);
      }
      targets[id] = target;
    }
  }
  using State = std::map<ObjectID, PointsToSet>;
  std::vector<State> out(p.blockCount() + 1);
  std::map<NodeID, PointsToSet> expected;
  bool changed = true;
  auto join = [](PointsToSet &a, const PointsToSet &b) {
    auto old = a.size();
    a.insert(b.begin(), b.end());
    return old != a.size();
  };
  while (changed) {
    changed = false;
    for (auto block : blocks) {
      State state;
      if (block == blocks.front())
        for (auto cell : cells)
          state[cell].insert(p.unknownObject());
      for (auto pred : blocks)
        for (auto succ : p.block(pred).successors)
          if (succ == block)
            for (const auto &entry : out[pred])
              join(state[entry.first], entry.second);
      for (auto id : p.block(block).accesses) {
        const auto &access = p.access(id);
        if (access.kind == Program::AccessKind::Store) {
          for (auto object : targets[id]) {
            if (strong && targets[id].size() == 1)
              state[object] = stored[id];
            else
              join(state[object], stored[id]);
          }
        } else {
          for (auto object : targets[id])
            changed |= join(expected[access.value], state[object]);
        }
      }
      if (state != out[block]) {
        out[block] = state;
        changed = true;
      }
    }
  }
  Solver::Config config;
  config.enableStrongUpdates = strong;
  Solver s(p, config);
  s.analyze();
  for (auto node : loads)
    expect(s, node, expected[node]);
}
} // namespace

TEST(ValueFlowGraphTest, StraightLineAndRerun) { straightLine(); }
TEST(ValueFlowGraphTest, Diamond) { diamond(); }
TEST(ValueFlowGraphTest, FullyDefinedDiamondDropsInitializer) {
  fullyDefinedDiamondDropsInitializer();
}
TEST(ValueFlowGraphTest, Loop) { loop(); }
TEST(ValueFlowGraphTest, WeakAliasing) { weakAliasing(); }
TEST(ValueFlowGraphTest, NullStoreKills) { nullStoreKills(); }
TEST(ValueFlowGraphTest, EscapeOrder) { escapeOrder(); }
TEST(ValueFlowGraphTest, InitializerEscapeOrder) { deepEscapeOrder(); }
TEST(ValueFlowGraphTest, ScalarCycleUsesWeakFallback) { scalarCycle(); }
TEST(ValueFlowGraphTest, HeapCycle) { heapCycle(); }
TEST(ValueFlowGraphTest, UnknownClobber) { unknownClobber(); }
TEST(ValueFlowGraphTest, ExplicitWeakStore) { explicitWeakStore(); }
TEST(ValueFlowGraphTest, IgnoresUnreachableBlocks) { unreachable(); }
TEST(ValueFlowGraphTest, EmptyBlockCycle) { emptyCycle(); }
TEST(ValueFlowGraphTest, InterproceduralSwap) { swapAcrossCall(); }
TEST(ValueFlowGraphTest, RecomputesWhenStrongBecomesWeak) {
  strongBecomesWeak();
}
TEST(ValueFlowGraphTest, ValidatesQueries) { queryValidation(); }

TEST(ValueFlowGraphTest, MatchesDenseReference) {
  for (unsigned seed = 0; seed < 500; ++seed) {
    SCOPED_TRACE(::testing::Message() << "seed " << seed);
    ASSERT_NO_THROW(differential(seed, true));
    ASSERT_NO_THROW(differential(seed, false));
  }
}

TEST(ValueFlowGraphTest, SharedProjectionKeepsDistinctInitialDefinitions) {
  Program p;
  const auto a = p.addObject(false, false, "a");
  const auto b = p.addObject(false, false, "b");
  const auto x = p.addObject(false, true, "x");
  const auto y = p.addObject(false, true, "y");
  p.setInitializer(x, p.address(a));
  p.setInitializer(y, p.address(b));
  const auto entry = p.addBlock();
  p.addRoot(entry);
  const auto pointer = p.addNode();
  p.addCopy(p.address(x), pointer);
  p.addCopy(p.address(y), pointer);
  const auto result = load(p, entry, pointer);
  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(result), (PointsToSet{a, b}));
  EXPECT_GT(solver.statistics().indirectFlowCacheHits, 0u);
}

TEST(ValueFlowGraphTest, CondensesWeakCyclesWithoutCrossingStrongKills) {
  for (bool strong : {false, true}) {
    Program p;
    const auto cell = p.addObject(strong, true, "cell");
    const auto loop = p.addBlock();
    p.addRoot(loop);
    p.addControlEdge(loop, loop);
    PointsToSet expected;
    ObjectID last = 0;
    for (unsigned index = 0; index < 40; ++index) {
      last = p.addObject(false, false, "value");
      expected.insert(last);
      if (index == 0)
        p.setInitializer(cell, p.address(last));
      p.addStore(loop, p.address(cell), p.address(last));
    }
    const auto result = load(p, loop, p.address(cell));
    Solver solver(p);
    solver.analyze();
    EXPECT_EQ(solver.pointsTo(result), strong ? PointsToSet{last} : expected);
    if (!strong) {
      EXPECT_GT(solver.statistics().projectedEvents, 40u);
      EXPECT_LT(solver.statistics().valueFlowJoinNodes, 8u);
    }
    for (ObjectID object = 1; object <= p.objectCount(); ++object)
      for (NodeID node : solver.pointedToBy(object))
        EXPECT_LE(node, p.nodeCount());
  }
}

TEST(ValueFlowGraphTest, MemoryFlowDoesNotExpandStoreLoadCrossProduct) {
  Program p;
  const auto cell = p.addObject(false, true, "cell");
  const auto loop = p.addBlock();
  p.addRoot(loop);
  p.addControlEdge(loop, loop);
  PointsToSet expected;
  std::vector<NodeID> loads;
  constexpr unsigned count = 64;
  for (unsigned index = 0; index < count; ++index) {
    const auto object = p.addObject(false, false, "value");
    expected.insert(object);
    if (index == 0)
      p.setInitializer(cell, p.address(object));
    p.addStore(loop, p.address(cell), p.address(object));
    loads.push_back(load(p, loop, p.address(cell)));
  }
  Solver solver(p);
  solver.analyze();
  for (NodeID result : loads)
    EXPECT_EQ(solver.pointsTo(result), expected);
  EXPECT_LT(solver.statistics().indirectEdges, count * 8u);
}

TEST(ValueFlowGraphTest, WeakProjectionPreservesOrderAndBranchReachability) {
  Program p;
  const auto a = p.addObject(false, false, "a");
  const auto b = p.addObject(false, false, "b");
  const auto dead = p.addObject(false, false, "dead");
  const auto cell = p.addObject(false, true, "cell");
  p.setInitializer(cell, p.address(a));
  const auto entry = p.addBlock(), left = p.addBlock(), right = p.addBlock();
  const auto join = p.addBlock();
  p.addRoot(entry);
  p.addControlEdge(entry, left);
  p.addControlEdge(entry, right);
  p.addControlEdge(left, join);
  p.addControlEdge(right, join);
  const auto before = load(p, entry, p.address(cell));
  p.addStore(left, p.address(cell), p.address(b));
  const auto onLeft = load(p, left, p.address(cell));
  const auto onRight = load(p, right, p.address(cell));
  const auto after = load(p, join, p.address(cell));
  p.addStore(join, p.address(cell), p.address(dead));
  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(before), (PointsToSet{a}));
  EXPECT_EQ(solver.pointsTo(onLeft), (PointsToSet{a, b}));
  EXPECT_EQ(solver.pointsTo(onRight), (PointsToSet{a}));
  EXPECT_EQ(solver.pointsTo(after), (PointsToSet{a, b}));
  EXPECT_EQ(solver.statistics().iteratedDominanceFrontierNodes, 0u);
}

TEST(ValueFlowGraphTest, SharedCalleeDoesNotDestroyUnrelatedLocalFlowSensitivity) {
  Program p;
  const auto a = p.addObject(false, false, "A");
  const auto b = p.addObject(false, false, "B");
  const auto mainFn = p.addFunction("main");
  const auto noopFn = p.addFunction("noop");
  const auto mainEntry = p.addBlock(mainFn);
  const auto noopEntry = p.addBlock(noopFn);
  p.setFunctionEntry(mainFn, mainEntry);
  p.setFunctionExit(mainFn, mainEntry);
  p.setFunctionEntry(noopFn, noopEntry);
  p.setFunctionExit(noopFn, noopEntry);
  p.addEntryFunction(mainFn);

  const auto x = p.addObject(true, true, "x");
  p.setEmptyInitializer(x);
  p.setObjectOwner(x, mainFn);

  p.addStore(mainEntry, p.address(x), p.address(a));
  const auto c1 = p.addCallSite(mainEntry);
  p.addCallTarget(c1, noopFn);
  const auto r1 = load(p, mainEntry, p.address(x));
  p.addStore(mainEntry, p.address(x), p.address(b));
  const auto c2 = p.addCallSite(mainEntry);
  p.addCallTarget(c2, noopFn);
  const auto r2 = load(p, mainEntry, p.address(x));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(r1), (PointsToSet{a}));
  EXPECT_EQ(solver.pointsTo(r2), (PointsToSet{b}));
  EXPECT_EQ(solver.statistics().interproceduralAuxEdges, 0u);
  EXPECT_FALSE(solver.statistics().usedWeakFallback);
}

TEST(ValueFlowGraphTest, ReadOnlyCalleePreservesCallerStrongUpdatesAcrossCalls) {
  Program p;
  const auto a = p.addObject(false, false, "A");
  const auto b = p.addObject(false, false, "B");
  const auto mainFn = p.addFunction("main");
  const auto readerFn = p.addFunction("reader");
  const auto mainEntry = p.addBlock(mainFn);
  const auto readerEntry = p.addBlock(readerFn);
  p.setFunctionEntry(mainFn, mainEntry);
  p.setFunctionExit(mainFn, mainEntry);
  p.setFunctionEntry(readerFn, readerEntry);
  p.setFunctionExit(readerFn, readerEntry);
  p.addEntryFunction(mainFn);

  const auto formalP = p.addNode();
  p.setFunctionParams(readerFn, {formalP});
  const auto observed = load(p, readerEntry, formalP);

  const auto x = p.addObject(true, true, "x");
  p.setEmptyInitializer(x);
  p.setObjectOwner(x, mainFn);

  p.addStore(mainEntry, p.address(x), p.address(a));
  const auto c1 = p.addCallSite(mainEntry, 0, 0, {p.address(x)});
  p.addCallTarget(c1, readerFn);
  const auto r1 = load(p, mainEntry, p.address(x));
  p.addStore(mainEntry, p.address(x), p.address(b));
  const auto c2 = p.addCallSite(mainEntry, 0, 0, {p.address(x)});
  p.addCallTarget(c2, readerFn);
  const auto r2 = load(p, mainEntry, p.address(x));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(r1), (PointsToSet{a}));
  EXPECT_EQ(solver.pointsTo(r2), (PointsToSet{b}));
  EXPECT_EQ(solver.pointsTo(observed), (PointsToSet{a, b}));
  EXPECT_FALSE(solver.statistics().usedWeakFallback);
}

TEST(ValueFlowGraphTest, PaperFigure1SwapExactInterprocedural) {
  // Exact multi-function graph from Figure 1(b) of Li et al. (ESEC/FSE 2011).
  Program p;
  const auto allocA = p.addObject(false, false, "alloc_A");
  const auto allocB = p.addObject(false, false, "alloc_B");
  const auto alloc_a = p.addObject(true, true, "alloc_a");
  const auto alloc_b = p.addObject(true, true, "alloc_b");
  p.setEmptyInitializer(alloc_a);
  p.setEmptyInitializer(alloc_b);

  const auto mainFn = p.addFunction("main");
  const auto swapFn = p.addFunction("swap");
  const auto bb0 = p.addBlock(mainFn);
  const auto bb1 = p.addBlock(mainFn);
  const auto bb2 = p.addBlock(mainFn);
  const auto bb3 = p.addBlock(mainFn);
  const auto bb4 = p.addBlock(swapFn);
  p.setFunctionEntry(mainFn, bb0);
  p.setFunctionExit(mainFn, bb3);
  p.setFunctionEntry(swapFn, bb4);
  p.setFunctionExit(swapFn, bb4);
  p.addEntryFunction(mainFn);
  p.addControlEdge(bb0, bb1);
  p.addControlEdge(bb0, bb2);
  p.addControlEdge(bb1, bb3);
  p.addControlEdge(bb2, bb3);

  const auto paramP = p.addNode();
  const auto paramQ = p.addNode();
  p.setFunctionParams(swapFn, {paramP, paramQ});

  p.addStore(bb0, p.address(alloc_a), p.address(allocA));
  p.addStore(bb0, p.address(alloc_b), p.address(allocB));
  const auto a1 = load(p, bb1, p.address(alloc_a));
  const auto cs = p.addCallSite(bb2, 0, 0, {p.address(alloc_a), p.address(alloc_b)});
  p.addCallTarget(cs, swapFn);
  const auto a2 = load(p, bb2, p.address(alloc_a));
  const auto a3 = load(p, bb3, p.address(alloc_a));

  const auto s1 = load(p, bb4, paramP);
  const auto s2 = load(p, bb4, paramQ);
  p.addStore(bb4, paramP, s2);
  p.addStore(bb4, paramQ, s1);

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(paramP), (PointsToSet{alloc_a}));
  EXPECT_EQ(solver.pointsTo(paramQ), (PointsToSet{alloc_b}));
  EXPECT_EQ(solver.pointsTo(a1), (PointsToSet{allocA}));
  EXPECT_EQ(solver.pointsTo(a2), (PointsToSet{allocB}));
  EXPECT_EQ(solver.pointsTo(a3), (PointsToSet{allocA, allocB}));
  EXPECT_EQ(solver.pointsTo(s1), (PointsToSet{allocA}));
  EXPECT_EQ(solver.pointsTo(s2), (PointsToSet{allocB}));
  EXPECT_GT(solver.statistics().interproceduralAuxEdges, 0u);
  EXPECT_FALSE(solver.statistics().usedWeakFallback);
}

TEST(ValueFlowGraphTest, MultiObjectHeapCycleDoesNotPrematurelyUnblockScalar) {
  // Two non-scalar heap objects form a 2-node escape cycle (cyc1 <-> cyc2).
  // Scalar `target` escapes into `chain1`, which is tied into the non-scalar
  // cycle; a strong update through `strongAlias` must be discovered before
  // `target` is finalized, without triggering any fallback.
  Program p;
  const auto va = p.addObject(false, false, "A");
  const auto vb = p.addObject(false, false, "B");
  const auto target = p.addObject(true, true, "target");
  const auto cyc1 = p.addObject(false, true, "cyc1");
  const auto cyc2 = p.addObject(false, true, "cyc2");
  const auto chain1 = p.addObject(false, true, "chain1");
  const auto chain2 = p.addObject(false, true, "chain2");
  p.setEmptyInitializer(target);
  p.setEmptyInitializer(cyc1);
  p.setEmptyInitializer(cyc2);
  p.setEmptyInitializer(chain1);
  p.setEmptyInitializer(chain2);
  const auto entry = p.addBlock();
  p.addRoot(entry);

  p.addStore(entry, p.address(cyc1), p.address(cyc2));
  p.addStore(entry, p.address(cyc2), p.address(cyc1));
  p.setInitializer(chain1, p.address(target));
  p.addStore(entry, p.address(chain2), load(p, entry, p.address(chain1)));
  p.addStore(entry, p.address(cyc1), p.address(chain1));
  p.addStore(entry, p.address(chain2), p.address(cyc2));

  const auto strongAlias = load(p, entry, p.address(chain1));
  p.addStore(entry, p.address(target), p.address(va));
  p.addStore(entry, strongAlias, p.address(vb));
  const auto out = load(p, entry, p.address(target));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(strongAlias), (PointsToSet{target}));
  EXPECT_EQ(solver.pointsTo(out), (PointsToSet{vb}));
  EXPECT_FALSE(solver.statistics().usedWeakFallback);
}

TEST(ValueFlowGraphTest, ScalarCycleWeakensOnlyCyclePreservingUnrelatedScalars) {
  Fixture f;
  auto &p = f.p;
  const auto y = p.addObject(true, true, "y");
  const auto z = p.addObject(true, true, "z");
  p.setInitializer(f.x, p.address(y));
  p.setInitializer(y, p.address(f.x));
  p.setEmptyInitializer(z);

  const auto xBefore = load(p, f.entry, p.address(f.x));
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  const auto xAfter = load(p, f.entry, p.address(f.x));

  // z is completely unrelated to the x <-> y scalar cycle and MUST retain
  // strong updates!
  p.addStore(f.entry, p.address(z), p.address(f.a));
  const auto zStrong = p.addStore(f.entry, p.address(z), p.address(f.b));
  const auto zAfter = load(p, f.entry, p.address(z));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(xBefore), (PointsToSet{y}));
  EXPECT_EQ(solver.pointsTo(xAfter), (PointsToSet{y, f.a}));
  EXPECT_EQ(solver.pointsTo(zAfter), (PointsToSet{f.b}));
  EXPECT_TRUE(solver.isStrongUpdate(zStrong));
  EXPECT_TRUE(solver.statistics().usedWeakFallback);
  EXPECT_EQ(solver.statistics().weakFallbackObjects, 2u);
  EXPECT_GT(solver.statistics().strongUpdateSites, 0u);
}

TEST(ValueFlowGraphTest, PermanentlyEmptyEscapePointerPreservesStrongUpdates) {
  Fixture f;
  auto &p = f.p;
  p.setEmptyInitializer(f.x);
  const auto deadPtr = p.addNode(); // never receives any object (pts == empty)
  p.addStore(f.entry, p.address(f.x), p.address(f.a));
  p.addStore(f.entry, deadPtr, p.address(f.x));
  const auto overwrite = p.addStore(f.entry, p.address(f.x), p.address(f.b));
  const auto result = load(p, f.entry, p.address(f.x));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(result), (PointsToSet{f.b}));
  EXPECT_TRUE(solver.isStrongUpdate(overwrite));
  EXPECT_FALSE(solver.statistics().usedWeakFallback);
}

TEST(ValueFlowGraphTest, DynamicIndirectCallResolutionAndVFGInspectionAPI) {
  Program p;
  const auto a = p.addObject(false, false, "A");
  const auto b = p.addObject(false, false, "B");
  const auto setObj = p.addObject(false, false, "set");
  const auto fpObj = p.addObject(true, true, "fp");
  const auto slot = p.addObject(true, true, "slot");
  p.setInitializer(fpObj, p.address(setObj));
  p.setEmptyInitializer(slot);

  const auto mainFn = p.addFunction("main");
  const auto setFn = p.addFunction("set");
  p.setFunctionObject(setObj, setFn);
  p.setAddressTakenFunction(setFn, true);

  const auto mainBB = p.addBlock(mainFn);
  const auto setBB = p.addBlock(setFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(setFn, setBB);
  p.setFunctionExit(setFn, setBB);
  p.addEntryFunction(mainFn);

  const auto formalP = p.addNode();
  p.setFunctionParams(setFn, {formalP});
  p.addStore(setBB, formalP, p.address(b));

  const auto initStore = p.addStore(mainBB, p.address(slot), p.address(a));
  const auto calleePtr = load(p, mainBB, p.address(fpObj));
  const auto cs = p.addCallSite(mainBB, calleePtr, 0, {p.address(slot)});
  const auto after = load(p, mainBB, p.address(slot));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(after), (PointsToSet{b}));
  EXPECT_EQ(solver.resolvedCallTargets(cs), (std::set<FunctionID>{setFn}));
  EXPECT_EQ(solver.statistics().dynamicIndirectTargets, 1u);
  EXPECT_FALSE(solver.statistics().usedWeakFallback);
  EXPECT_TRUE(solver.isFunctionReachable(mainFn));
  EXPECT_TRUE(solver.isFunctionReachable(setFn));
  EXPECT_TRUE(solver.isBlockReachable(setBB));

  // Verify VFG inspection APIs.
  EXPECT_GT(solver.vfgNodeCount(), p.nodeCount());
  EXPECT_FALSE(solver.isJoinNode(p.address(a)));
  const auto storedNode = p.access(initStore).value;
  EXPECT_TRUE(solver.vfgSuccessors(p.address(a)).count(storedNode));
  EXPECT_FALSE(solver.vfgIndirectSuccessors(p.address(a)).count(storedNode));
}

TEST(ValueFlowGraphTest, PerFunctionIncrementalReuseOnRecomputation) {
  Program p;
  const auto a = p.addObject(false, false, "A");
  const auto b = p.addObject(false, false, "B");
  const auto mainFn = p.addFunction("main");
  const auto stableFn = p.addFunction("stable");
  const auto changingFn = p.addFunction("changing");
  const auto mainBB = p.addBlock(mainFn);
  const auto stableBB = p.addBlock(stableFn);
  const auto changingBB = p.addBlock(changingFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(stableFn, stableBB);
  p.setFunctionExit(stableFn, stableBB);
  p.setFunctionEntry(changingFn, changingBB);
  p.setFunctionExit(changingFn, changingBB);
  p.addEntryFunction(mainFn);

  const auto cell = p.addObject(true, true, "cell");
  const auto heap = p.addObject(false, true, "heap");
  const auto extra = p.addObject(true, true, "extra");
  p.setEmptyInitializer(cell);
  p.setEmptyInitializer(extra);
  p.setInitializer(heap, p.address(heap));
  p.addStore(mainBB, p.address(heap), p.address(extra));

  // stableFn loads cell; changingFn initially strongly updates cell, then
  // becomes weak when heap cycle adds extra into alias.
  const auto stableRead = load(p, stableBB, p.address(cell));
  const auto alias = load(p, changingBB, p.address(heap));
  p.addCopy(p.address(cell), alias);
  p.addStore(changingBB, alias, p.address(b));

  p.addStore(mainBB, p.address(cell), p.address(a));
  const auto c1 = p.addCallSite(mainBB);
  p.addCallTarget(c1, changingFn);
  const auto c2 = p.addCallSite(mainBB);
  p.addCallTarget(c2, stableFn);

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(stableRead), (PointsToSet{a, b}));
  EXPECT_GT(solver.statistics().functionFlowReuseHits, 0u);
}

TEST(ValueFlowGraphTest, InterproceduralRandomMatchesDenseOracle) {
  // Independent dense context-insensitive interprocedural reference oracle over
  // 50 random 3-function programs (main calling f1 and f2 at multiple sites,
  // with globals, local slots, strong/weak stores, and read-only callees).
  for (unsigned seed = 0; seed < 50; ++seed) {
    SCOPED_TRACE(::testing::Message() << "interprocedural seed " << seed);
    std::mt19937 rng(seed + 1000);
    Program p;
    std::vector<ObjectID> globals, vals;
    std::vector<bool> isSingleton;
    for (unsigned i = 0; i < 3; ++i) {
      const bool single = (rng() % 2 == 0);
      auto g = p.addObject(single, true);
      p.setEmptyInitializer(g);
      globals.push_back(g);
      isSingleton.push_back(single);
    }
    for (unsigned i = 0; i < 3; ++i)
      vals.push_back(p.addObject(false, false));

    const auto mainFn = p.addFunction("main");
    const auto f1 = p.addFunction("f1");
    const auto f2 = p.addFunction("f2");
    std::vector<FunctionID> fns{mainFn, f1, f2};
    std::vector<BlockID> entries, exits;
    for (FunctionID fn : fns) {
      auto e = p.addBlock(fn);
      auto x = p.addBlock(fn);
      p.setFunctionEntry(fn, e);
      p.setFunctionExit(fn, x);
      p.addControlEdge(e, x);
      entries.push_back(e);
      exits.push_back(x);
    }
    p.addEntryFunction(mainFn);

    // Local scalar in main that neither f1 nor f2 ever accesses.
    const auto localOnly = p.addObject(true, true, "localOnly");
    p.setEmptyInitializer(localOnly);
    p.setObjectOwner(localOnly, mainFn);
    p.addStore(entries[0], p.address(localOnly), p.address(vals[0]));
    const auto localBefore = load(p, entries[0], p.address(localOnly));

    enum class OpKind { Store, Load, Call };
    struct Op {
      OpKind kind;
      unsigned target = 0; // global idx or callee idx (1 or 2)
      ObjectID storedVal = 0;
      NodeID loadNode = 0;
    };
    std::vector<std::vector<Op>> fnOps(3);

    // f1: modifies globals[0], reads globals[1]
    {
      const ObjectID sv = vals[rng() % 3];
      p.addStore(entries[1], p.address(globals[0]), p.address(sv));
      fnOps[1].push_back({OpKind::Store, 0, sv, 0});
      const NodeID ld = load(p, entries[1], p.address(globals[1]));
      fnOps[1].push_back({OpKind::Load, 1, 0, ld});
    }
    // f2: reads globals[0], optionally modifies globals[2]
    const bool f2ModifiesG2 = (rng() % 2 == 1);
    {
      const NodeID ld = load(p, entries[2], p.address(globals[0]));
      fnOps[2].push_back({OpKind::Load, 0, 0, ld});
      if (f2ModifiesG2) {
        const ObjectID sv = vals[rng() % 3];
        p.addStore(entries[2], p.address(globals[2]), p.address(sv));
        fnOps[2].push_back({OpKind::Store, 2, sv, 0});
      }
    }

    // main: interleave stores, loads, and multiple calls to f1 and f2.
    for (unsigned step = 0; step < 6; ++step) {
      const unsigned op = rng() % 4;
      if (op == 0) {
        const unsigned gi = rng() % 3;
        const ObjectID sv = vals[rng() % 3];
        p.addStore(entries[0], p.address(globals[gi]), p.address(sv));
        fnOps[0].push_back({OpKind::Store, gi, sv, 0});
      } else if (op == 1) {
        const unsigned gi = rng() % 3;
        const NodeID ld = load(p, entries[0], p.address(globals[gi]));
        fnOps[0].push_back({OpKind::Load, gi, 0, ld});
      } else if (op == 2) {
        const auto cs = p.addCallSite(entries[0]);
        p.addCallTarget(cs, f1);
        fnOps[0].push_back({OpKind::Call, 1, 0, 0});
      } else {
        const auto cs = p.addCallSite(entries[0]);
        p.addCallTarget(cs, f2);
        fnOps[0].push_back({OpKind::Call, 2, 0, 0});
      }
    }
    p.addStore(exits[0], p.address(localOnly), p.address(vals[1]));
    const auto localAfter = load(p, exits[0], p.address(localOnly));

    // Independent dense context-insensitive interprocedural fixed-point oracle:
    // merges callee entry states across callsites, propagates exit states back
    // ONLY for globals modified by the callee (strong overwrite if singleton
    // global, matching PreprocessGlobalFlow!).
    using MemState = std::array<PointsToSet, 3>;
    auto joinSets = [](PointsToSet &dst, const PointsToSet &src) {
      const auto old = dst.size();
      dst.insert(src.begin(), src.end());
      return dst.size() != old;
    };
    auto calleeModifies = [&](unsigned callee, unsigned gi) {
      return (callee == 1 && gi == 0) ||
             (callee == 2 && gi == 2 && f2ModifiesG2);
    };
    std::array<MemState, 3> fnIn{};
    std::array<MemState, 3> fnOut{};
    std::array<bool, 3> fnCalled{true, false, false};
    std::map<NodeID, PointsToSet> expected;
    bool changed = true;
    while (changed) {
      changed = false;
      for (unsigned fi = 0; fi < 3; ++fi) {
        if (!fnCalled[fi])
          continue;
        MemState st = fnIn[fi];
        for (const auto &op : fnOps[fi]) {
          if (op.kind == OpKind::Store) {
            if (isSingleton[op.target]) {
              st[op.target].clear();
              st[op.target].insert(op.storedVal);
            } else {
              st[op.target].insert(op.storedVal);
            }
          } else if (op.kind == OpKind::Load) {
            changed |= joinSets(expected[op.loadNode], st[op.target]);
          } else {
            const unsigned c = op.target;
            if (!fnCalled[c]) {
              fnCalled[c] = true;
              changed = true;
            }
            for (unsigned gi = 0; gi < 3; ++gi)
              changed |= joinSets(fnIn[c][gi], st[gi]);
            for (unsigned gi = 0; gi < 3; ++gi) {
              if (!calleeModifies(c, gi))
                continue;
              if (isSingleton[gi])
                st[gi] = fnOut[c][gi];
              else
                joinSets(st[gi], fnOut[c][gi]);
            }
          }
        }
        for (unsigned gi = 0; gi < 3; ++gi)
          changed |= joinSets(fnOut[fi][gi], st[gi]);
      }
    }

    Solver solver(p);
    solver.analyze();
    // Unrelated localOnly MUST remain exact despite multiple calls to f1/f2!
    EXPECT_EQ(solver.pointsTo(localBefore), (PointsToSet{vals[0]}));
    EXPECT_EQ(solver.pointsTo(localAfter), (PointsToSet{vals[1]}));
    for (const auto &[ld, want] : expected)
      EXPECT_EQ(solver.pointsTo(ld), want);
    EXPECT_FALSE(solver.statistics().usedWeakFallback);
  }
}

TEST(ValueFlowGraphTest, CallsiteScopedPreprocessGlobalFlowIsolatesSharedCalls) {
  // Paper Algorithm 2 line 8: swap(&a, &b); a = &E; swap(&c, &d);
  // Because `a` is NOT passed at the second swap callsite (and does not escape
  // globally), the second swap must NOT leak `E` backward into the first swap
  // or clobber `a` after `a = &E`.
  Program p;
  const auto A = p.addObject(false, false, "A");
  const auto B = p.addObject(false, false, "B");
  const auto C = p.addObject(false, false, "C");
  const auto D = p.addObject(false, false, "D");
  const auto E = p.addObject(false, false, "E");

  const auto mainFn = p.addFunction("main");
  const auto swapFn = p.addFunction("swap");
  const auto mainBB = p.addBlock(mainFn);
  const auto swapBB = p.addBlock(swapFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(swapFn, swapBB);
  p.setFunctionExit(swapFn, swapBB);
  p.addEntryFunction(mainFn);

  const auto a = p.addObject(true, true, "a");
  const auto b = p.addObject(true, true, "b");
  const auto c = p.addObject(true, true, "c");
  const auto d = p.addObject(true, true, "d");
  for (ObjectID slot : {a, b, c, d}) {
    p.setEmptyInitializer(slot);
    p.setObjectOwner(slot, mainFn);
  }

  const auto paramP = p.addNode();
  const auto paramQ = p.addNode();
  p.setFunctionParams(swapFn, {paramP, paramQ});
  const auto s1 = load(p, swapBB, paramP);
  const auto s2 = load(p, swapBB, paramQ);
  p.addStore(swapBB, paramP, s2);
  p.addStore(swapBB, paramQ, s1);

  p.addStore(mainBB, p.address(a), p.address(A));
  p.addStore(mainBB, p.address(b), p.address(B));
  p.addStore(mainBB, p.address(c), p.address(C));
  p.addStore(mainBB, p.address(d), p.address(D));

  const auto call1 = p.addCallSite(mainBB, 0, 0, {p.address(a), p.address(b)});
  p.addCallTarget(call1, swapFn);
  const auto afterFirst = load(p, mainBB, p.address(a));

  p.addStore(mainBB, p.address(a), p.address(E));
  const auto call2 = p.addCallSite(mainBB, 0, 0, {p.address(c), p.address(d)});
  p.addCallTarget(call2, swapFn);
  const auto afterSecond = load(p, mainBB, p.address(a));

  Solver solver(p);
  solver.analyze();
  EXPECT_FALSE(solver.pointsTo(afterFirst).count(E));
  EXPECT_EQ(solver.pointsTo(afterSecond), (PointsToSet{E}));
  EXPECT_FALSE(solver.statistics().usedWeakFallback);
}

TEST(ValueFlowGraphTest, CallsiteAnchoredInsertionsPreserveIntraBlockOrder) {
  Program p;
  const auto A = p.addObject(false, false, "A");
  const auto B = p.addObject(false, false, "B");
  const auto mainFn = p.addFunction("main");
  const auto calleeFn = p.addFunction("callee");
  const auto mainBB = p.addBlock(mainFn);
  const auto calleeBB = p.addBlock(calleeFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(calleeFn, calleeBB);
  p.setFunctionExit(calleeFn, calleeBB);
  p.addEntryFunction(mainFn);

  const auto slot = p.addObject(true, true, "slot");
  const auto copy = p.addObject(false, true, "copy");
  p.setEmptyInitializer(slot);
  p.setEmptyInitializer(copy);
  p.setObjectOwner(slot, mainFn);
  p.setObjectOwner(copy, calleeFn);

  const auto calleeRead = load(p, calleeBB, p.address(copy));

  p.addStore(mainBB, p.address(slot), p.address(A));
  const auto cs = p.addCallSite(mainBB);
  p.addCallTarget(cs, calleeFn);
  const auto afterCall = load(p, mainBB, p.address(slot));
  p.addStore(mainBB, p.address(slot), p.address(B));

  // Late-anchor a byval copy BEFORE `cs`, and an external write AFTER `cs`
  // (before `afterCall`).
  const auto tmp = p.addNode();
  p.addLoadBeforeCall(cs, p.address(slot), tmp);
  p.addStoreBeforeCall(cs, p.address(copy), tmp, false);
  p.addStoreAfterCall(cs, p.address(slot), p.address(B), false);

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(calleeRead), (PointsToSet{A}));
  EXPECT_EQ(solver.pointsTo(afterCall), (PointsToSet{A, B}));
}

TEST(ValueFlowGraphTest, CallBoundaryArrivalAtSecondCallsitePropagatesFlow) {
  // Verify callsite-scoped PreprocessGlobalFlow re-evaluates an already-
  // processed object when it later arrives at a SECOND callsite's actual arg.
  Program p;
  const auto A = p.addObject(false, false, "A");
  const auto B = p.addObject(false, false, "B");

  const auto mainFn = p.addFunction("main");
  const auto readFn = p.addFunction("read");
  const auto writeFn = p.addFunction("write");
  const auto mainBB = p.addBlock(mainFn);
  const auto readBB = p.addBlock(readFn);
  const auto writeBB = p.addBlock(writeFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(readFn, readBB);
  p.setFunctionExit(readFn, readBB);
  p.setFunctionEntry(writeFn, writeBB);
  p.setFunctionExit(writeFn, writeBB);
  p.addEntryFunction(mainFn);

  // `slot` is a non-global local of `main`; `box` holds &slot and is loaded
  // after `slot` has already reached `call1(slot)`'s boundary!
  const auto slot = p.addObject(true, true, "slot");
  const auto box = p.addObject(false, true, "box");
  p.setEmptyInitializer(slot);
  p.setEmptyInitializer(box);
  p.setObjectOwner(slot, mainFn);
  p.setObjectOwner(box, mainFn);

  const auto readParam = p.addNode();
  p.setFunctionParams(readFn, {readParam});
  const auto observed = load(p, readBB, readParam);

  const auto writeParam = p.addNode();
  p.setFunctionParams(writeFn, {writeParam});
  p.addStore(writeBB, writeParam, p.address(B));

  // Direct store &A into slot, then store &slot into non-singleton box.
  p.addStore(mainBB, p.address(slot), p.address(A));
  p.addStore(mainBB, p.address(box), p.address(slot));

  // First call passes &slot directly (objReachesCallBoundary[slot] becomes true).
  const auto c1 = p.addCallSite(mainBB, 0, 0, {p.address(slot)});
  p.addCallTarget(c1, readFn);

  // Second call passes pointer loaded from `box` (receives `slot` only after
  // `box` is processed!).
  const auto latePtr = load(p, mainBB, p.address(box));
  const auto c2 = p.addCallSite(mainBB, 0, 0, {latePtr});
  p.addCallTarget(c2, writeFn);

  const auto afterWrite = load(p, mainBB, p.address(slot));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(observed), (PointsToSet{A}));
  EXPECT_EQ(solver.pointsTo(afterWrite), (PointsToSet{B}));
}

TEST(ValueFlowGraphTest, NonGlobalCalleeInitializerSurvivesCallerPruning) {
  // Non-global, non-owned object (e.g., calloc) with explicit initializer
  // dereferenced strictly inside a callee whose caller does not carry it.
  Program p;
  const auto mainFn = p.addFunction("main");
  const auto calleeFn = p.addFunction("callee");
  const auto mainBB = p.addBlock(mainFn);
  const auto calleeBB = p.addBlock(calleeFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(calleeFn, calleeBB);
  p.setFunctionExit(calleeFn, calleeBB);
  p.addEntryFunction(mainFn);

  const auto heap = p.addObject(false, true, "callocObj");
  p.setObjectGlobal(heap, false);
  p.setInitializer(heap, p.nullValue());

  const auto loaded = load(p, calleeBB, p.address(heap));
  const auto cs = p.addCallSite(mainBB);
  p.addCallTarget(cs, calleeFn);

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(loaded), (PointsToSet{p.nullObject()}));
}

TEST(ValueFlowGraphTest, TwoHopTransitiveEscapeInvalidatesCallsiteScoping) {
  // local_a escapes into global G (never directly reaching a call boundary).
  // After `slot` is processed, `slot` escapes into local_a via a late heap
  // load; `slot` must become globally visible across callsites.
  Program p;
  const auto A = p.addObject(false, false, "A");
  const auto B = p.addObject(false, false, "B");

  const auto mainFn = p.addFunction("main");
  const auto writerFn = p.addFunction("writer");
  const auto mainBB = p.addBlock(mainFn);
  const auto writerBB = p.addBlock(writerFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(writerFn, writerBB);
  p.setFunctionExit(writerFn, writerBB);
  p.addEntryFunction(mainFn);

  const auto G = p.addObject(false, true, "G");
  const auto localA = p.addObject(false, true, "localA");
  const auto slot = p.addObject(true, true, "slot");
  const auto relay = p.addObject(false, true, "relay");
  p.setEmptyInitializer(G);
  p.setEmptyInitializer(localA);
  p.setEmptyInitializer(slot);
  p.setEmptyInitializer(relay);
  p.setObjectOwner(localA, mainFn);
  p.setObjectOwner(slot, mainFn);
  p.setObjectOwner(relay, mainFn);

  // writerFn reaches `slot` strictly through global G -> localA -> slot.
  const auto pLocalA = load(p, writerBB, p.address(G));
  const auto pSlot = load(p, writerBB, pLocalA);
  p.addStore(writerBB, pSlot, p.address(B));

  // main stores &localA into G, stores &localA into relay, then stores &slot
  // into *relay (creating a 2-hop escape slot -> localA -> G).
  p.addStore(mainBB, p.address(G), p.address(localA));
  p.addStore(mainBB, p.address(relay), p.address(localA));
  const auto loadedLocalA = load(p, mainBB, p.address(relay));
  p.addStore(mainBB, loadedLocalA, p.address(slot));

  p.addStore(mainBB, p.address(slot), p.address(A));
  const auto cs = p.addCallSite(mainBB); // no args, void return!
  p.addCallTarget(cs, writerFn);
  const auto afterCall = load(p, mainBB, p.address(slot));

  Solver solver(p);
  solver.analyze();
  EXPECT_TRUE(solver.pointsTo(afterCall).count(B));
}

TEST(ValueFlowGraphTest, SecondIndirectTargetStrongStoreTriggersCleanRestart) {
  // Indirect callsite initially resolves to non-modifying `noop`, then later
  // resolves to `writer` which strongly writes `slot`. Should trigger a clean
  // strong restart without weakening `slot`.
  Program p;
  const auto A = p.addObject(false, false, "A");
  const auto B = p.addObject(false, false, "B");
  const auto C = p.addObject(false, false, "C");

  const auto noopObj = p.addObject(false, false, "noop");
  const auto writerObj = p.addObject(false, false, "writer");
  const auto fpSlot = p.addObject(false, true, "fpSlot");
  const auto slot = p.addObject(true, true, "slot");
  p.setInitializer(fpSlot, p.address(noopObj));
  p.setEmptyInitializer(slot);

  const auto mainFn = p.addFunction("main");
  const auto noopFn = p.addFunction("noop");
  const auto writerFn = p.addFunction("writer");
  p.setFunctionObject(noopObj, noopFn);
  p.setFunctionObject(writerObj, writerFn);
  p.setAddressTakenFunction(noopFn, true);
  p.setAddressTakenFunction(writerFn, true);

  const auto mainBB = p.addBlock(mainFn);
  const auto noopBB = p.addBlock(noopFn);
  const auto writerBB = p.addBlock(writerFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(noopFn, noopBB);
  p.setFunctionExit(noopFn, noopBB);
  p.setFunctionEntry(writerFn, writerBB);
  p.setFunctionExit(writerFn, writerBB);
  p.addEntryFunction(mainFn);

  // writerFn overwrites slot strongly twice: B then C; local load must see {C}.
  p.addStore(writerBB, p.address(slot), p.address(B));
  p.addStore(writerBB, p.address(slot), p.address(C));
  const auto insideWriter = load(p, writerBB, p.address(slot));

  // main stores writerObj into fpSlot via a load from another non-singleton
  // container so noopObj reaches fp first and writerObj arrives later.
  const auto box = p.addObject(false, true, "box");
  p.setInitializer(box, p.address(writerObj));
  const auto lateFn = load(p, mainBB, p.address(box));
  p.addStore(mainBB, p.address(fpSlot), lateFn);

  p.addStore(mainBB, p.address(slot), p.address(A));
  const auto fp = load(p, mainBB, p.address(fpSlot));
  p.addCallSite(mainBB, fp);
  const auto afterCall = load(p, mainBB, p.address(slot));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(insideWriter), (PointsToSet{C}));
  EXPECT_EQ(solver.pointsTo(afterCall), (PointsToSet{A, C}));
  EXPECT_FALSE(solver.statistics().usedWeakFallback);
}

TEST(ValueFlowGraphTest, DynamicIndirectCalleeMutatingViaFormalParamCleanRestart) {
  // Singleton `slot` is emitted BEFORE `fp` resolves to `writer(p) { *p = &B; *p = &C; }`.
  // When `fp` resolves, `writer` receives `&slot` via formal parameter `p`.
  // Calling computePointedToBy() before the strongRestartNeeded check detects
  // the new strong stores through `p` and performs a clean strong restart
  // instead of falling back to local weakening.
  Program p;
  const auto A = p.addObject(false, false, "A");
  const auto B = p.addObject(false, false, "B");
  const auto C = p.addObject(false, false, "C");

  const auto writerObj = p.addObject(false, false, "writer");
  const auto slot = p.addObject(true, true, "slot");
  const auto fpBox = p.addObject(false, true, "fpBox");
  const auto fpMid = p.addObject(false, true, "fpMid");
  p.setEmptyInitializer(slot);
  p.setInitializer(fpBox, p.address(writerObj));
  p.setEmptyInitializer(fpMid);

  const auto mainFn = p.addFunction("main");
  const auto writerFn = p.addFunction("writer");
  p.setFunctionObject(writerObj, writerFn);
  p.setAddressTakenFunction(writerFn, true);

  const auto mainBB = p.addBlock(mainFn);
  const auto writerBB = p.addBlock(writerFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(writerFn, writerBB);
  p.setFunctionExit(writerFn, writerBB);
  p.addEntryFunction(mainFn);

  // writerFn strongly updates *param twice (B then C).
  const auto param = p.addNode();
  p.setFunctionParams(writerFn, {param});
  p.addStore(writerBB, param, p.address(B));
  p.addStore(writerBB, param, p.address(C));
  const auto insideWriter = load(p, writerBB, param);

  // Two-hop non-singleton chain delays `fp` resolution until after `slot`
  // could already be emitted.
  const auto f1 = load(p, mainBB, p.address(fpBox));
  p.addStore(mainBB, p.address(fpMid), f1);
  const auto fp = load(p, mainBB, p.address(fpMid));

  p.addStore(mainBB, p.address(slot), p.address(A));
  const auto beforeCall = load(p, mainBB, p.address(slot));
  p.addCallSite(mainBB, fp, 0, {p.address(slot)});
  const auto afterCall = load(p, mainBB, p.address(slot));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(beforeCall), (PointsToSet{A}));
  EXPECT_EQ(solver.pointsTo(insideWriter), (PointsToSet{C}));
  EXPECT_EQ(solver.pointsTo(afterCall), (PointsToSet{C}));
  EXPECT_FALSE(solver.statistics().usedWeakFallback);
}

TEST(ValueFlowGraphTest, DeadEmptyPointerDoesNotInvertAcyclicEscapeOrder) {
  // Acyclic escape order A ✂ B ✂ C with ObjectID(C) < ObjectID(A).
  // A escapes into a genuinely dead pointer (deadPtr), while C escapes into
  // `p = load(&A)` which is only temporarily empty until A is processed.
  // breakBlockedSingletons() must process A first (provably dead empty ptr),
  // preserving full precision on C without triggering weak fallback.
  Program p;
  const auto V1 = p.addObject(false, false, "V1");
  const auto V2 = p.addObject(false, false, "V2");
  const auto C = p.addObject(true, true, "C");
  const auto A = p.addObject(true, true, "A");
  const auto B = p.addObject(true, true, "B");
  p.setEmptyInitializer(C);
  p.setEmptyInitializer(A);
  p.setEmptyInitializer(B);

  const auto entry = p.addBlock();
  p.addRoot(entry);

  const auto deadPtr = p.addNode(); // never receives any object
  p.addStore(entry, deadPtr, p.address(A));
  p.addStore(entry, p.address(A), p.address(B));
  const auto pLoad = load(p, entry, p.address(A)); // temporarily empty
  p.addStore(entry, pLoad, p.address(C));
  const auto qLoad = load(p, entry, p.address(B)); // will point to C

  p.addStore(entry, p.address(C), p.address(V1));
  const auto strongKill = p.addStore(entry, qLoad, p.address(V2));
  const auto res = load(p, entry, p.address(C));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(qLoad), (PointsToSet{C}));
  EXPECT_EQ(solver.pointsTo(res), (PointsToSet{V2}));
  EXPECT_TRUE(solver.isStrongUpdate(strongKill));
  EXPECT_FALSE(solver.statistics().usedWeakFallback);
}

TEST(ValueFlowGraphTest, ScalarCycleThroughEmptyLoadPreservesDownstreamScalar) {
  // X <-> X2 form a scalar cycle, while Y (with lower ObjectID than X) escapes
  // into `p = load(&X)` (initially empty). Implicit load dependency Y -> X
  // must ensure only {X, X2} are weakened while Y keeps its strong updates.
  Program p;
  const auto V1 = p.addObject(false, false, "V1");
  const auto V2 = p.addObject(false, false, "V2");
  const auto Y = p.addObject(true, true, "Y");
  const auto X = p.addObject(true, true, "X");
  const auto X2 = p.addObject(true, true, "X2");
  p.setEmptyInitializer(Y);
  p.setInitializer(X, p.address(X2));
  p.setInitializer(X2, p.address(X));

  const auto entry = p.addBlock();
  p.addRoot(entry);

  const auto pFromX = load(p, entry, p.address(X));
  p.addStore(entry, pFromX, p.address(Y));

  p.addStore(entry, p.address(Y), p.address(V1));
  const auto yStrong = p.addStore(entry, p.address(Y), p.address(V2));
  const auto yRes = load(p, entry, p.address(Y));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(yRes), (PointsToSet{V2}));
  EXPECT_TRUE(solver.isStrongUpdate(yStrong));
  EXPECT_TRUE(solver.statistics().usedWeakFallback);
  EXPECT_EQ(solver.statistics().weakFallbackObjects, 2u);
}

TEST(ValueFlowGraphTest, ExternalEscapeDisablesCallsitePruningCleanly) {
  // Local singleton `slot` escapes externally via addExternalEscape().
  // Subsequent call to no-arg defined wrapper `trigger()` (which stores through
  // unknown) must NOT be pruned for `slot`, while unrelated local `safe` stays
  // unaffected and `slot` itself is NOT injected into its own points-to set.
  Program p;
  const auto A = p.addObject(false, false, "A");
  const auto B = p.addObject(false, false, "B");

  const auto mainFn = p.addFunction("main");
  const auto triggerFn = p.addFunction("trigger");
  const auto mainBB = p.addBlock(mainFn);
  const auto triggerBB = p.addBlock(triggerFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(triggerFn, triggerBB);
  p.setFunctionExit(triggerFn, triggerBB);
  p.addEntryFunction(mainFn);

  const auto slot = p.addObject(true, true, "slot");
  const auto safe = p.addObject(true, true, "safe");
  p.setEmptyInitializer(slot);
  p.setEmptyInitializer(safe);
  p.setObjectOwner(slot, mainFn);
  p.setObjectOwner(safe, mainFn);

  p.addStore(triggerBB, p.unknownValue(), p.unknownValue(),
             /*allowStrong=*/false);

  p.addExternalEscape(mainBB, p.address(slot));
  p.addStore(mainBB, p.address(slot), p.address(A));
  p.addStore(mainBB, p.address(slot), p.address(B)); // strong kill before call
  p.addStore(mainBB, p.address(safe), p.address(A));
  const auto cs = p.addCallSite(mainBB);
  p.addCallTarget(cs, triggerFn);
  const auto slotAfter = load(p, mainBB, p.address(slot));
  const auto safeAfter = load(p, mainBB, p.address(safe));

  Solver solver(p);
  solver.analyze();
  EXPECT_EQ(solver.pointsTo(slotAfter), (PointsToSet{B, p.unknownObject()}));
  EXPECT_EQ(solver.pointsTo(safeAfter), (PointsToSet{A}));
  EXPECT_FALSE(solver.statistics().usedWeakFallback);
}

TEST(ValueFlowGraphTest, SVFGNodeBS256BitChunkOperationsAndBoundaryIds) {
  using lotus::analysis::SVFGNodeBS;
  const std::vector<std::uint32_t> boundaryA = {
      0, 1, 63, 64, 127, 128, 191, 192, 255, 256, 511, 512, 1024, 4096};
  const std::vector<std::uint32_t> boundaryB = {
      1, 64, 128, 255, 257, 512, 768, 4096, 8192};

  SVFGNodeBS a;
  for (std::uint32_t id : boundaryA)
    EXPECT_TRUE(a.insertBit(id));
  for (std::uint32_t id : boundaryA)
    EXPECT_FALSE(a.insertBit(id));
  EXPECT_EQ(a.size(), boundaryA.size());

  SVFGNodeBS b;
  b.insertSorted(boundaryB);
  EXPECT_EQ(b.size(), boundaryB.size());

  EXPECT_TRUE(a.intersects(b));
  SVFGNodeBS disjoint;
  disjoint.insertBit(2);
  disjoint.insertBit(258);
  disjoint.insertBit(9000);
  EXPECT_FALSE(a.intersects(disjoint));

  std::vector<std::uint32_t> inter;
  a.forEachIn(b, [&](std::uint32_t id) { inter.push_back(id); });
  EXPECT_EQ(inter, (std::vector<std::uint32_t>{1, 64, 128, 255, 512, 4096}));

  std::vector<std::uint32_t> onlyInA;
  a.forEachNotIn(b, [&](std::uint32_t id) { onlyInA.push_back(id); });
  EXPECT_EQ(onlyInA,
            (std::vector<std::uint32_t>{0, 63, 127, 191, 192, 256, 511, 1024}));

  SVFGNodeBS merged = a;
  SVFGNodeBS diff;
  EXPECT_TRUE(merged.uniteWithDiff(b, diff));
  EXPECT_EQ(diff, (SVFGNodeBS{257, 768, 8192}));
  EXPECT_TRUE(a.isSubsetOf(merged));
  EXPECT_TRUE(b.isSubsetOf(merged));
  EXPECT_FALSE(merged.isSubsetOf(a));

  SVFGNodeBS merged2;
  merged2.unite(b);
  merged2.unite(a);
  EXPECT_EQ(merged, merged2);
  EXPECT_EQ(merged.hash(), merged2.hash());

  EXPECT_TRUE(merged.subtract(b));
  EXPECT_EQ(merged, (SVFGNodeBS{0, 63, 127, 191, 192, 256, 511, 1024}));
  EXPECT_EQ(merged.erase(256), 1u);
  EXPECT_EQ(merged.erase(256), 0u);
  EXPECT_FALSE(merged.count(256));
}

TEST(ValueFlowGraphTest, ReadOnlyConstantGlobalIgnoresStoresAndFeedsLoads) {
  Program p;
  const auto A = p.addObject(false, false, "A");
  const auto B = p.addObject(false, false, "B");
  const auto C = p.addObject(false, false, "C");

  const auto ro1 = p.addObject(false, true, "ro1");
  const auto ro2 = p.addObject(false, true, "ro2");
  const auto roEmpty = p.addObject(false, true, "roEmpty");
  const auto mutGlob = p.addObject(false, true, "mutGlob");
  p.setObjectReadOnly(ro1, true);
  p.setObjectReadOnly(ro2, true);
  p.setObjectReadOnly(roEmpty, true);
  p.setInitializer(ro1, p.address(A));
  p.setInitializer(ro2, p.address(B));
  p.setEmptyInitializer(roEmpty);
  p.setInitializer(mutGlob, p.address(A));

  const auto fn = p.addFunction("main");
  const auto bb = p.addBlock(fn);
  p.setFunctionEntry(fn, bb);
  p.setFunctionExit(fn, bb);
  p.addEntryFunction(fn);

  // Wildcard unknown store + direct store through a shared pointer to ro1/ro2.
  p.addStore(bb, p.unknownValue(), p.unknownValue(), /*allowStrong=*/false);
  const auto sharedPtr = p.addNode();
  p.addCopy(p.address(ro1), sharedPtr);
  p.addCopy(p.address(ro2), sharedPtr);
  p.addCopy(p.address(roEmpty), sharedPtr);
  p.addStore(bb, sharedPtr, p.address(C), /*allowStrong=*/false);

  const auto l1 = load(p, bb, sharedPtr);
  const auto l2 = load(p, bb, sharedPtr);
  const auto lEmpty = load(p, bb, p.address(roEmpty));
  const auto lMut = load(p, bb, p.address(mutGlob));
  const auto lUnk = load(p, bb, p.unknownValue());

  Solver solver(p);
  solver.analyze();
  // Read-only globals ignore direct & unknown stores and share their load fan.
  EXPECT_EQ(solver.pointsTo(l1), (PointsToSet{A, B}));
  EXPECT_EQ(solver.pointsTo(l2), (PointsToSet{A, B}));
  EXPECT_TRUE(solver.pointsTo(lEmpty).empty());
  // Mutable global receives wildcard unknown store.
  EXPECT_EQ(solver.pointsTo(lMut), (PointsToSet{A, p.unknownObject()}));
  // Wildcard load soundly observes explicit read-only initializers.
  EXPECT_TRUE(solver.pointsTo(lUnk).count(A));
  EXPECT_TRUE(solver.pointsTo(lUnk).count(B));
}

TEST(ValueFlowGraphTest, OnlineCopySCCCondensationAndCompactedHooksExact) {
  // Build > 1024 VFG nodes with a large copy cycle containing dereference,
  // store-through, external-escape, and indirect-call hooks so
  // condenseVFGSCCs(), PendingHookBatch, hook compaction, and wave merge
  // caches all exercise on the cycle.
  Program p;
  const auto A = p.addObject(false, false, "A");
  const auto B = p.addObject(false, false, "B");
  const auto calleeObj = p.addObject(false, false, "callee");
  const auto targetSlot = p.addObject(false, true, "targetSlot");
  const auto escSlot = p.addObject(true, true, "escSlot");
  p.setEmptyInitializer(targetSlot);
  p.setEmptyInitializer(escSlot);

  const auto mainFn = p.addFunction("main");
  const auto calleeFn = p.addFunction("callee");
  p.setFunctionObject(calleeObj, calleeFn);
  p.setAddressTakenFunction(calleeFn, true);
  const auto mainBB = p.addBlock(mainFn);
  const auto calleeBB = p.addBlock(calleeFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(calleeFn, calleeBB);
  p.setFunctionExit(calleeFn, calleeBB);
  p.addEntryFunction(mainFn);

  // Callee stores &B into targetSlot.
  p.addStore(calleeBB, p.address(targetSlot), p.address(B), false);

  // Construct a 1100-node copy ring so condenseVFGSCCs() triggers (>= 1024).
  std::vector<NodeID> ring;
  ring.reserve(1100);
  for (int i = 0; i < 1100; ++i)
    ring.push_back(p.addNode());
  for (std::size_t i = 0; i < ring.size(); ++i)
    p.addCopy(ring[i], ring[(i + 1) % ring.size()]);

  // Seed &targetSlot, &calleeObj, and &A into different nodes on the ring.
  p.addCopy(p.address(targetSlot), ring[0]);
  p.addCopy(p.address(calleeObj), ring[300]);
  p.addCopy(p.address(escSlot), ring[600]);
  p.addCopy(p.address(A), ring[900]);

  // Attach diverse hooks around the ring.
  p.addExternalEscape(mainBB, ring[100]);
  p.addStore(mainBB, ring[200], p.address(A), /*allowStrong=*/false);
  const auto loadedBefore = load(p, mainBB, ring[400]);
  const auto cs = p.addCallSite(mainBB, ring[700]);
  const auto loadedAfter = load(p, mainBB, ring[800]);

  Solver solver(p);
  solver.analyze();

  EXPECT_TRUE(solver.isFunctionReachable(calleeFn));
  EXPECT_EQ(solver.resolvedCallTargets(cs), (std::set<FunctionID>{calleeFn}));
  EXPECT_TRUE(solver.pointsTo(ring[0]).count(targetSlot));
  EXPECT_TRUE(solver.pointsTo(ring[500]).count(calleeObj));
  EXPECT_TRUE(solver.pointsTo(ring[1099]).count(A));
  EXPECT_TRUE(solver.pointsTo(loadedBefore).count(A));
  EXPECT_FALSE(solver.pointsTo(loadedBefore).count(B));
  EXPECT_TRUE(solver.pointsTo(loadedAfter).count(A));
  EXPECT_TRUE(solver.pointsTo(loadedAfter).count(B));
  EXPECT_TRUE(solver.pointedToBy(targetSlot).count(ring[400]));
  EXPECT_TRUE(solver.pointedToBy(targetSlot).count(ring[800]));
  EXPECT_TRUE(solver.vfgSuccessors(ring[0]).count(ring[1]));
}

TEST(ValueFlowGraphTest,
     MultiFunctionWeakObjectsRemainExactWithoutPromotionBounds) {
  // With all lossy active-function promotion bounds removed, multiple weak
  // objects accessed across many functions plus a trailing `load unknown` ->
  // `store unknown` sequence must retain exact flow-sensitive per-object
  // precision (no flow-insensitive weak-cell collapse or cross-object merging).
  Program p;
  const auto mainFn = p.addFunction("main");
  const auto mainBB = p.addBlock(mainFn);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.addEntryFunction(mainFn);

  std::vector<ObjectID> payloads;
  std::vector<ObjectID> cells;
  for (int i = 0; i < 12; ++i) {
    payloads.push_back(p.addObject(false, false, "P" + std::to_string(i)));
    const auto cell = p.addObject(false, true, "C" + std::to_string(i));
    p.setEmptyInitializer(cell);
    cells.push_back(cell);
  }

  std::vector<NodeID> helperLoads;
  for (int f = 0; f < 5; ++f) {
    const auto hFn = p.addFunction("h" + std::to_string(f));
    const auto hBB = p.addBlock(hFn);
    p.setFunctionEntry(hFn, hBB);
    p.setFunctionExit(hFn, hBB);
    for (int i = 0; i < 12; ++i) {
      p.addStore(hBB, p.address(cells[i]), p.address(payloads[i]), false);
      helperLoads.push_back(load(p, hBB, p.address(cells[i])));
    }
    const auto cs = p.addCallSite(mainBB);
    p.addCallTarget(cs, hFn);
  }

  // Trailing unknown load + store after all helper calls: unkLoaded sees all
  // stored payloads, but helperLoads (which execute strictly before the store)
  // must NOT see other cells' payloads.
  const auto unkLoaded = load(p, mainBB, p.unknownValue());
  p.addStore(mainBB, p.unknownValue(), unkLoaded, /*allowStrong=*/false);

  Solver solver(p);
  solver.analyze();

  for (int i = 0; i < 12; ++i) {
    EXPECT_TRUE(solver.pointsTo(helperLoads[i]).count(payloads[i]));
    EXPECT_FALSE(solver.pointsTo(helperLoads[i]).count(payloads[(i + 1) % 12]));
    EXPECT_TRUE(solver.pointsTo(unkLoaded).count(payloads[i]));
  }
}

TEST(ValueFlowGraphTest, DynamicUnknownExternalTargetAndLazyResolvedTargets) {
  // Verify setUnknownExternalTarget resolves dynamically when calleePointer
  // gains `unknown` (with and without argument escape) and resolvedCallTargets
  // lazily includes concrete address-taken targets even when siteCallsUnknownHub
  // is active.
  Program p;
  const auto A = p.addObject(false, false, "A");
  const auto B = p.addObject(false, false, "B");
  const auto atObj = p.addObject(false, false, "atFn");

  const auto mainFn = p.addFunction("main");
  const auto atFn = p.addFunction("atFn");
  const auto unkWritable = p.addFunction("<unk_rw>");
  const auto unkReadOnly = p.addFunction("<unk_ro>");
  p.setFunctionObject(atObj, atFn);
  p.setAddressTakenFunction(atFn, true);

  const auto mainBB = p.addBlock(mainFn);
  const auto atBB = p.addBlock(atFn);
  const auto rwBB = p.addBlock(unkWritable);
  const auto roBB = p.addBlock(unkReadOnly);
  p.setFunctionEntry(mainFn, mainBB);
  p.setFunctionExit(mainFn, mainBB);
  p.setFunctionEntry(atFn, atBB);
  p.setFunctionExit(atFn, atBB);
  p.setFunctionEntry(unkWritable, rwBB);
  p.setFunctionExit(unkWritable, rwBB);
  p.setFunctionEntry(unkReadOnly, roBB);
  p.setFunctionExit(unkReadOnly, roBB);
  p.addEntryFunction(mainFn);

  p.addStore(rwBB, p.unknownValue(), p.address(B), /*allowStrong=*/false);
  p.setFunctionReturn(unkWritable, p.unknownValue());
  p.setFunctionReturn(unkReadOnly, p.address(A));

  const auto localEsc = p.addObject(true, true, "localEsc");
  const auto localSafe = p.addObject(true, true, "localSafe");
  p.setEmptyInitializer(localEsc);
  p.setEmptyInitializer(localSafe);
  p.setObjectOwner(localEsc, mainFn);
  p.setObjectOwner(localSafe, mainFn);
  p.addStore(mainBB, p.address(localEsc), p.address(A));
  p.addStore(mainBB, p.address(localSafe), p.address(A));

  const auto fp = p.addNode();
  p.addCopy(p.unknownValue(), fp);
  p.addCopy(p.address(atObj), fp);

  const auto resRW = p.addNode();
  const auto csRW = p.addCallSite(mainBB, fp, resRW, {p.address(localEsc)});
  p.setUnknownExternalTarget(csRW, unkWritable, /*escapeArgs=*/true);

  const auto resRO = p.addNode();
  const auto csRO = p.addCallSite(mainBB, fp, resRO, {p.address(localSafe)});
  p.setUnknownExternalTarget(csRO, unkReadOnly, /*escapeArgs=*/false);

  const auto afterEsc = load(p, mainBB, p.address(localEsc));
  const auto afterSafe = load(p, mainBB, p.address(localSafe));

  Solver solver(p);
  solver.analyze();

  EXPECT_TRUE(solver.isFunctionReachable(unkWritable));
  EXPECT_TRUE(solver.isFunctionReachable(unkReadOnly));
  EXPECT_TRUE(solver.isFunctionReachable(atFn));
  // Lazy resolvedCallTargets must report concrete target `atFn` as well.
  EXPECT_TRUE(solver.resolvedCallTargets(csRW).count(atFn));
  EXPECT_TRUE(solver.resolvedCallTargets(csRW).count(unkWritable));
  // localEsc escaped via csRW so unkWritable's `store unknown, &B` reaches it.
  EXPECT_TRUE(solver.pointsTo(afterEsc).count(A));
  EXPECT_TRUE(solver.pointsTo(afterEsc).count(B));
  // localSafe passed only to readonly csRO; still keeps A.
  EXPECT_TRUE(solver.pointsTo(afterSafe).count(A));
  EXPECT_TRUE(solver.pointsTo(resRW).count(p.unknownObject()));
  EXPECT_TRUE(solver.pointsTo(resRO).count(A));
}
