#include "Alias/InclusionBased/FlowSensitive/ValueFlow/ValueFlowGraph.h"

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
